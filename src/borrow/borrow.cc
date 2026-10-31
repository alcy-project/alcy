// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "borrow/borrow.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "borrow/diag_code.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/debug/profiler/profiler.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "lowering/lowering.h"

namespace borrow {

namespace {

constexpr u32 NO_ROOT = 0xFFFFFFFFu;

// A path step is either a field index or a dereference. Reading a place
// of reference type steps through to the referent, which is what makes
// the referent itself nameable: `*b` and `b.field` are `b`'s path plus
// a dereference, so a loan of one is seen to overlap a store to the
// other. A field index can never collide with the marker because the
constexpr u32 DEREF_STEP = 0xFFFFFFFFu;

// A path step for a buffer element. The index is a runtime value, so
// the step names no particular element; `steps_match` treats it as
// matching every index, which is what makes a reallocating write
// conflict with a loan into the old block. Two loans into distinct
// elements of one buffer therefore overlap too: the source never tied
// them to specific elements, so it cannot claim they are disjoint.
constexpr u32 ELEMENT_STEP = 0xFFFFFFFEu;

// A place: a root register (alloca or block parameter) plus a path.
// Moves, borrows, and revives name overlapping places: one path prefixes
// the other (or they are equal). The path is a window on the checker's
// arena, which outlives every place a function builds: naming one copies
// three words, and the steps are appended to the arena once.
struct Place {
  u32 root = NO_ROOT;
  u32 head = 0;
  u32 len = 0;
};

// Whether a value of the type can carry a loan: references and
// views directly, aggregates through their fields, and function
// values through the environments they close over. Anything else
// is loan-free, so tracking it would only constrain dead air.
// Runs on cycle-free storage like is_copy_type: value cycles never
// reach here, and references short-circuit instead of recursing.
bool value_may_carry(const ir::Storage& storage, ir::TypeIdx type) {
  const ir::TypeNode& node = storage.types()[type.idx];
  switch (node.tag) {
    case ir::TypeTag::Ref:
    case ir::TypeTag::MutRef:
    case ir::TypeTag::Str:
    case ir::TypeTag::Slice:
    case ir::TypeTag::Func: return true;
    case ir::TypeTag::Struct: {
      const ir::StructType& shape = storage.struct_types()[node.as_struct()];
      for (ir::TypeIdx field : shape.fields) {
        if (value_may_carry(storage, field)) {
          return true;
        }
      }
      return false;
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& shape = storage.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : shape.elements) {
        if (value_may_carry(storage, element)) {
          return true;
        }
      }
      return false;
    }
    case ir::TypeTag::Array:
      return value_may_carry(storage,
                             storage.array_types()[node.as_array()].element);
    case ir::TypeTag::Enum: {
      const ir::EnumType& shape = storage.enum_types()[node.as_enum()];
      for (ir::EnumVariantTypeIdx vidx = shape.variants.head();
           vidx.idx < shape.variants.head().idx + shape.variants.size();
           vidx = ir::EnumVariantTypeIdx(vidx.idx + 1)) {
        for (ir::TypeIdx field : storage.enum_variant_types()[vidx].fields) {
          if (value_may_carry(storage, field)) {
            return true;
          }
        }
      }
      return false;
    }
    default: return false;
  }
}

bool steps_match(u32 a, u32 b) {
  // An element step stands for whichever element a runtime index named,
  // so it matches any index and any other element step. Comparing two
  // element steps as equal would report a conflict between loans into
  // distinct elements of one buffer, which the source never tied
  // together.
  if (a == ELEMENT_STEP || b == ELEMENT_STEP) {
    return true;
  }
  return a == b;
}

bool overlaps(const Place& a, const Place& b, std::span<const u32> steps) {
  if (a.root != b.root) {
    return false;
  }
  const u32 common = a.len < b.len ? a.len : b.len;
  for (u32 i = 0; i < common; ++i) {
    if (!steps_match(steps[a.head + i], steps[b.head + i])) {
      return false;
    }
  }
  return true;
}

struct Loan {
  u32 reg = NO_ROOT;
  Place place;
  bool exclusive = false;
  // The instruction that created the loan. A check never sees a loan
  // that is not born yet.
  u32 birth = 0;
  // Summary token for a parameter alloca: carries the parameter
  // index so return-reachability becomes a summary. Never conflicts;
  // reification propagates the caller's own loans instead.
  u32 param = NO_ROOT;
};

// One way a returned reference reaches a parameter: which parameter,
// and the path from that parameter to the reference. An empty path is
// the parameter itself; a field index names a projection through it.
// Callers keep every entry alive, which is the conjunction the region
// model requires of conditional joins and struct composition alike.
struct SummaryEntry {
  u32 param = NO_ROOT;
  std::vector<u32> path;
  bool exclusive = false;
};

// A register's loan set: a window on the checker's arena. Naming a row copies
// another row's window instead of its loans, and appending to a shared window
// first moves it to the arena's end, so a function's flow costs an append
// rather than a heap block per register it names.
struct FlowRow {
  std::vector<u32>* arena = nullptr;
  u32 head = 0;
  u32 len = 0;

  // A window moves as the arena grows, so the iterator reads through the row
  // rather than caching the arena's data.
  class Iter {
   public:
    Iter(const FlowRow& row, u32 pos) : row_(row), pos_(pos) {}
    u32 operator*() const { return (*row_.arena)[row_.head + pos_]; }
    Iter& operator++() {
      ++pos_;
      return *this;
    }
    bool operator!=(const Iter& other) const { return pos_ != other.pos_; }

   private:
    const FlowRow& row_;
    u32 pos_;
  };

  Iter begin() const { return Iter(*this, 0); }
  Iter end() const { return Iter(*this, len); }
  void clear() { len = 0; }

  void push_back(u32 loan) {
    std::vector<u32>& steps = *arena;
    if (head + len == steps.size()) {
      steps.push_back(loan);
      ++len;
      return;
    }
    const u32 moved = static_cast<u32>(steps.size());
    steps.resize(moved + len + 1);
    for (u32 i = 0; i < len; ++i) {
      steps[moved + i] = steps[head + i];
    }
    steps[moved + len] = loan;
    head = moved;
    ++len;
  }
};

class Checker {
 public:
  Checker(const lowering::LoweredPackage& lowered,
          const ir::Storage& storage,
          diag::DiagBag& bag,
          ir::SymbolTable& strings,
          debug::Profiler* profiler = nullptr)
      : lowered(lowered),
        storage(storage),
        bag(bag),
        strings(strings),
        profiler(profiler) {}

  const lowering::LoweredPackage& lowered;
  const ir::Storage& storage;
  diag::DiagBag& bag;
  ir::SymbolTable& strings;
  // Where the trace events go, or nothing. The two sweeps - summaries,
  // then the check - run function by function on the caller's thread,
  // and each function body gets its own region under "borrow".
  debug::Profiler* profiler = nullptr;

  // Per-function scratch, indexed by global register.
  std::vector<u32> home;
  // A place path is a handful of steps, and forward names one per
  // instruction that carries a place, so a row is a window on a shared arena
  // rather than a vector of its own: naming a path appends its steps and the
  // row holds where they start, which keeps the copy off the heap.
  std::vector<u32> path_steps_;
  std::vector<u32> path_head;
  std::vector<u32> path_len;
  std::vector<u32> flow_steps;
  std::vector<FlowRow> flow;
  std::vector<Loan> loans;
  // Two-phase borrows: an exclusive receiver whose only use is the call
  // it serves. The borrow is a reservation until the call activates it,
  // so the argument evaluation between the two is the window ADR-0012
  // opens for reads of the place.
  std::vector<u8> reserved_receiver;
  // What the package named, by the address it named, and the captured
  // addresses among it. Both were walks over every name in the package: one
  // per diagnostic, and one per function of every sweep.
  struct NamedAddr {
    std::string_view name;
    // A register can be named more than once, and a parameter is one if any
    // of its names says so.
    bool is_param = false;
  };
  std::unordered_map<u32, NamedAddr> addr_by_register_;
  std::vector<u32> captures_;
  // Scratch for the sweeps: the sets a block's liveness is built in, the
  // successors of a block, and the walks' stacks. Each is cleared where it is
  // used, so the capacity one sweep needed is the capacity the next reuses.
  std::vector<u32> live_in_scratch_;
  std::vector<u32> live_out_scratch_;
  // The loans one block's live-out set holds, marked by stamp. Asking
  // whether a loan is live on exit by scanning that set made the sweep's
  // inner loop as long as the set, once per loan of the function.
  std::vector<u32> live_mark;
  u32 live_stamp = 0;
  std::vector<ir::BlockIdx> succs_scratch_;
  std::vector<ir::BlockIdx> stack_scratch_;
  std::vector<ir::BlockIdx> post_scratch_;
  // The block order the check pass walks, and the state it reads a block
  // with. Both are rebuilt for every function, and both keep the capacity
  // the widest function needed rather than returning it to the heap.
  std::vector<ir::BlockIdx> block_order_;
  std::vector<Place> state_scratch_;
  // The two states the may-move fixpoint exchanges with its rows.
  std::vector<Place> move_in_scratch_;
  std::vector<Place> move_out_scratch_;
  std::vector<u32> use_count_scratch_;
  std::vector<u8> receiver_call_scratch_;
  // Entry-block allocas holding a block parameter, paired with that
  // parameter's position. Filled by param_allocas per function and read
  // by is_param_root, so both see the same homes.
  std::vector<std::pair<u32, u32>> param_homes;
  // Move states per block, indexed by global block. Joins union
  // predecessor states (a use after a maybe-move is an error);
  // sibling branches stay independent through CFG predecessors.
  // Checks seed from moved_in (before the block's own moves).
  std::vector<std::vector<Place>> moved_in;
  std::vector<std::vector<Place>> moved_out;
  // Function summaries (return-regions): the ways a returned
  // reference reaches the parameters, indexed by function. Call sites
  // reify each entry against the matching argument.
  std::vector<std::vector<SummaryEntry>> summaries;
  // Liveness, per block and per loan: the loans still live where a
  // block is entered and where it is left, and the last instruction of
  // a block that reads a loan. A loan's extent is a region rather than
  // a range, so a loan live on one branch is not live on its siblings.
  std::vector<std::vector<u32>> live_in;
  std::vector<std::vector<u32>> live_out;
  // The last-use rows are a few words per block, so a vector per block would
  // spend an allocation on every block to hold them. They are held flat
  // instead: block b's row is loan_stride words at b * loan_stride.
  std::vector<u32> loan_last_use;
  u32 loan_stride = 0;
  // CFG shape for the two fixpoints, and the first and last instruction of
  // each block. Indexed by global block like the rows above, and filled by
  // build_preds and compute_liveness for one function at a time.
  std::vector<std::vector<ir::BlockIdx>> preds;
  // Successors of every block of the function being fixed, held flat: block
  // b's row is succ_len[b] entries at succ_head[b]. A terminator does not
  // change while its function is checked, so each sweep of the backward
  // fixpoint reads the rows instead of re-deriving the targets.
  std::vector<ir::BlockIdx> succ_flat;
  std::vector<u32> succ_head;
  std::vector<u32> succ_len;
  std::vector<u32> block_first;
  std::vector<u32> block_last;
  // Visit marks for the block walk, stamped per call so the table is sized
  // once and never cleared.
  std::vector<u32> walk_mark;
  u32 walk_stamp = 0;
  // The range of registers each function names, so that clearing one walks
  // its registers rather than its instructions.
  std::vector<u32> fn_reg_lo;
  std::vector<u32> fn_reg_hi;
  // Whether a type carries a reference is a property of the type, and the
  // module's types do not change while it is checked, so the walk over the
  // shape of a type runs once and every load or indirect result that asks
  // reads the answer. 0 is unanswered, 1 says no, 2 says yes.
  std::vector<u8> carry_memo_;

  std::span<const u32> steps_of(u32 reg) const {
    return std::span<const u32>(path_steps_.data() + path_head[reg],
                                path_len[reg]);
  }

  std::span<const u32> steps_of(const Place& place) const {
    return std::span<const u32>(path_steps_.data() + place.head, place.len);
  }

  // Names a window the checker already holds for a register, without copying
  // its steps: both rows read the same steps from then on.
  void set_row(u32 reg, const Place& place) {
    path_head[reg] = place.head;
    path_len[reg] = place.len;
  }

  // A place whose window is another place's extended by the steps a summary
  // recorded.
  Place extend_place(const Place& base, std::span<const u32> steps) {
    const u32 head = static_cast<u32>(path_steps_.size());
    path_steps_.resize(head + base.len);
    for (u32 i = 0; i < base.len; ++i) {
      path_steps_[head + i] = path_steps_[base.head + i];
    }
    path_steps_.insert(path_steps_.end(), steps.begin(), steps.end());
    Place place;
    place.root = base.root;
    place.head = head;
    place.len = base.len + static_cast<u32>(steps.size());
    return place;
  }

  void clear_path(u32 reg) { path_len[reg] = 0; }

  void set_step(u32 reg, u32 step) {
    path_head[reg] = static_cast<u32>(path_steps_.size());
    path_len[reg] = 1;
    path_steps_.push_back(step);
  }

  // Naming a path the arena already holds: the steps are copied by index, so
  // growing the arena cannot move the source out from under the copy.
  void copy_path(u32 dst, u32 src) {
    if (dst == src) {
      return;
    }
    const u32 head = path_head[src];
    const u32 len = path_len[src];
    const u32 new_head = static_cast<u32>(path_steps_.size());
    path_steps_.resize(new_head + len);
    for (u32 i = 0; i < len; ++i) {
      path_steps_[new_head + i] = path_steps_[head + i];
    }
    path_head[dst] = new_head;
    path_len[dst] = len;
  }

  // Naming a path held outside the arena: a loan's place, or one a summary
  // projection wrote.
  void set_path(u32 dst, std::span<const u32> steps) {
    path_head[dst] = static_cast<u32>(path_steps_.size());
    path_len[dst] = static_cast<u32>(steps.size());
    path_steps_.insert(path_steps_.end(), steps.begin(), steps.end());
  }

  void push_path_step(u32 reg, u32 step) {
    const u32 head = path_head[reg];
    const u32 len = path_len[reg];
    // A row still at the arena's end grows in place; any other has its steps
    // written again behind the new one.
    if (static_cast<usize>(head) + len == path_steps_.size()) {
      path_steps_.push_back(step);
      path_len[reg] = len + 1;
      return;
    }
    const u32 new_head = static_cast<u32>(path_steps_.size());
    path_steps_.resize(new_head + len + 1);
    for (u32 i = 0; i < len; ++i) {
      path_steps_[new_head + i] = path_steps_[head + i];
    }
    path_steps_[new_head + len] = step;
    path_head[reg] = new_head;
    path_len[reg] = len + 1;
  }

  bool carries_reference(ir::TypeIdx type) {
    u8& memo = carry_memo_[type.idx];
    if (memo == 0) {
      memo = value_may_carry(storage, type) ? 2 : 1;
    }
    return memo == 2;
  }

  const ir::Instruction& instr_at(ir::InstructionIdx idx) const {
    return storage.instrs()[idx];
  }

  ir::TypeTag tag_of(ir::TypeIdx idx) const { return storage.types()[idx].tag; }

  // The trace name for a function: its source spelling. Profiles by
  // what the borrow checker checked, so two functions that share a
  // spelling - instantiations of one generic - read as repeat reports
  // of the same name rather than as one function's work.
  [[nodiscard]] std::string_view fn_name(const ir::Function& fn) const {
    return strings.get(fn.meta.name);
  }

  std::string_view addr_name(u32 reg) const {
    const auto found = addr_by_register_.find(reg);
    return found == addr_by_register_.end() ? "value" : found->second.name;
  }

  bool is_param_root(u32 reg, const ir::Function& fn) const {
    for (ir::BlockParamIdx pidx :
         storage.blocks()[fn.blocks.head()].block_params) {
      if (storage.block_params()[pidx].reg.idx == reg) {
        return true;
      }
    }
    const auto named = addr_by_register_.find(reg);
    if (named != addr_by_register_.end() && named->second.is_param) {
      return true;
    }
    for (const auto& home_entry : param_homes) {
      if (home_entry.first == reg) {
        return true;
      }
    }
    return false;
  }

  // Resolves an address operand to its place. Returns false for
  // non-address values (immediates, computed temporaries).
  bool place_of(const ir::Operand& operand, Place& place) const {
    if (!operand.is<ir::RegisterIdx>()) {
      return false;
    }
    const u32 reg = operand.as_register().idx;
    if (reg >= home.size() || home[reg] == NO_ROOT) {
      return false;
    }
    place.root = home[reg];
    place.head = path_head[reg];
    place.len = path_len[reg];
    return true;
  }

  // Seeds one summary token per parameter alloca before analysis.
  // Parameters arrive already borrowed; the token makes their flow
  // to the return observable without creating conflicts.
  void seed_param_loans(const ir::Function& fn) {
    param_allocas(fn, param_homes);
    for (const auto& [alloca, index] : param_homes) {
      if (alloca >= flow.size()) {
        continue;
      }
      Place place;
      place.root = alloca;
      loans.push_back({alloca, place, false, 0, index});
      flow[alloca].push_back(static_cast<u32>(loans.size() - 1));
    }
  }

  // Names a borrowed capture's place as the referent behind its pointer:
  // the environment holds a reference, so the name stands for what the
  // reference points at, with the dereference in the path. That is what lets
  // a move through the capture be told from a move of an owned value. A moved
  // Copy capture is owned storage and needs none of this.
  void seed_capture(u32 addr) {
    home[addr] = addr;
    set_step(addr, DEREF_STEP);
  }

  // Seeds every capture once, when the scratch is sized; the seeding below
  // covers the ones reset_function clears.
  void seed_all_captures() {
    for (u32 addr : captures_) {
      seed_capture(addr);
    }
  }

  // Re-seeds the captures inside a function's register range, which
  // reset_function cleared with the rest of it. Seeding every capture of the
  // package in every function made the sweep quadratic in the package's
  // closure count.
  void seed_capture_places(u32 reg_lo, u32 reg_hi) {
    auto it = std::lower_bound(captures_.begin(), captures_.end(), reg_lo);
    for (; it != captures_.end() && *it < reg_hi; ++it) {
      seed_capture(*it);
    }
  }

  void forward(ir::FunctionIdx fidx) {
    const ir::Function& fn = storage.functions()[fidx];
    seed_param_loans(fn);
    seed_capture_places(fn_reg_lo[fidx.idx], fn_reg_hi[fidx.idx]);
    // Every alloca names its own storage, whichever block holds it.
    // A block is not visited in execution order, so a use in an early
    // block would otherwise miss an alloca defined in a later one.
    for (ir::BlockIdx bidx : fn.blocks) {
      for (ir::InstructionIdx iidx : storage.blocks()[bidx].instrs) {
        const ir::Instruction& instr = instr_at(iidx);
        if (instr.op == ir::Opcode::Alloca && instr.dst.is_valid() &&
            instr.dst.idx < home.size()) {
          home[instr.dst.idx] = instr.dst.idx;
        }
      }
    }
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
      for (ir::InstructionIdx iidx : block.instrs) {
        forward_instr(iidx);
      }
    }
  }

  void forward_instr(ir::InstructionIdx iidx) {
    const ir::Instruction& instr = instr_at(iidx);
    const u32 pos = iidx.idx;
    auto operand_reg = [&](u32 offset, u32& reg) {
      if (offset >= instr.operands.size()) {
        return false;
      }
      const ir::Operand& operand =
          storage.operands()[instr.operands.head() + offset];
      if (!operand.is<ir::RegisterIdx>()) {
        return false;
      }
      reg = operand.as_register().idx;
      return reg < home.size();
    };
    switch (instr.op) {
      case ir::Opcode::Alloca: {
        if (instr.dst.is_valid() && instr.dst.idx < home.size()) {
          home[instr.dst.idx] = instr.dst.idx;
        }
        break;
      }
      case ir::Opcode::Load: {
        u32 addr = NO_ROOT;
        if (!operand_reg(0, addr) || !instr.dst.is_valid()) {
          break;
        }
        // Reading a place of reference type yields the referent, and the
        // referent is a place: home/path carry the read place plus a
        // dereference, and the loan flows on as a reborrow. An
        // aggregate that holds a reference behaves the same way, because
        // loading `H { r: &mut i32 }` copies a reference that still
        // points into the loan. A load of anything else is a *copy*
        // that owns nothing, so the loan stops there: `x := *r` must
        // not look like returning the reference, nor keep the borrow
        // alive past its last use.
        const ir::TypeIdx loaded_ty = storage.registers()[instr.dst.idx].type;
        const ir::TypeTag loaded = tag_of(loaded_ty);
        const bool is_reborrow =
            loaded == ir::TypeTag::Ref || loaded == ir::TypeTag::MutRef;
        // A `str` is a view into someone's buffer, so a copy of one
        // keeps the buffer's loan alive exactly like a copy of a
        // reference does. Without this a view outlives the push that
        // reallocates under it: the loan dies at the copy and the
        // conflict is never reported. An aggregate of scalars - a
        // `Point`, a tuple, an `Option<i32>` - carries nothing, so
        // reading one out of a borrowed place owns its copy outright:
        // `d := v.pop()` on a `Vec<Point>` must not read as a borrow
        // of `v` that refuses the next `v.push`. The predicate is the
        // one an indirect call's result already tracks through.
        const bool loaded_carries = carries_reference(loaded_ty);
        if (loaded_carries) {
          flow[instr.dst.idx] = flow[addr];
        } else {
          flow[instr.dst.idx].clear();
        }
        if (is_reborrow) {
          // Reading a reference is not a copy of a pointer: it is a
          // loan on the place the reference points into, so the
          // referent is the place of the loan the reference stands
          // for. Naming the slot that holds the reference instead
          // gives two references to one place two loans that do not
          // overlap, so nothing reports them aliasing and an exclusive
          // loan can be taken from a shared one. A reference with no
          // loan behind it - a parameter of unknown origin - keeps the
          // slot, which names the referent only by convention. When a
          // call hands back a reference derived from more than one
          // parameter the summary does not say which place it names,
          // so the first loan it carried is the referent; the rule
          // matrix cannot observe the difference, and a summary that
          // could say would remove the choice.
          Place referent;
          for (u32 loan : flow[instr.dst.idx]) {
            if (loan < loans.size() && loans[loan].place.root != NO_ROOT) {
              referent = loans[loan].place;
              break;
            }
          }
          if (referent.root != NO_ROOT) {
            home[instr.dst.idx] = referent.root;
            set_row(instr.dst.idx, referent);
          } else if (home[addr] != NO_ROOT) {
            home[instr.dst.idx] = home[addr];
            copy_path(instr.dst.idx, addr);
          } else {
            break;
          }
          push_path_step(instr.dst.idx, DEREF_STEP);
        }
        break;
      }
      case ir::Opcode::ExtractValue: {
        // A field projected out of a slice value names the same place:
        // the extracted buffer pointer is usable as the buffer it
        // points at. Anything else keeps today's behavior, where a
        // copied-out field owns nothing: extending liveness there
        // would keep an aggregate's loan alive past a plain field
        // copy, which rejects correct programs.
        u32 src = NO_ROOT;
        if (!operand_reg(0, src) || !instr.dst.is_valid()) {
          break;
        }
        const ir::TypeIdx src_type = storage.registers()[src].type;
        const ir::TypeTag src_tag = tag_of(src_type);
        // A `str` is a view like a slice: its first field is the
        // buffer it reads through, so the buffer half carries the loan
        // the view stands for. Reading it as length owns nothing.
        bool is_slice_value =
            src_tag == ir::TypeTag::Slice || src_tag == ir::TypeTag::Str;
        if (src_tag == ir::TypeTag::Ref || src_tag == ir::TypeTag::MutRef) {
          const ir::TypeIdx pointee =
              storage.ref_types()[storage.types()[src_type].as_ref()].pointee;
          const ir::TypeTag pointee_tag = tag_of(pointee);
          is_slice_value = pointee_tag == ir::TypeTag::Slice ||
                           pointee_tag == ir::TypeTag::Str;
        }
        if (!is_slice_value) {
          break;
        }
        // Only the buffer half stands for the place the view reads
        // through. Copying the length out is a plain integer copy that
        // owns nothing, and letting it keep the loan alive would make
        // `n := slice_len(s)` count as a use of `s`.
        u32 field = 0;
        bool have_field = false;
        if (instr.operands.size() >= 2) {
          const ir::Operand& index_op =
              storage.operands()[instr.operands.head() + 1];
          if (index_op.is<ir::ImmutableIdx>()) {
            const ir::Immutable& imm =
                storage.immutables()[index_op.as_immutable()];
            const ir::TypeTag tag = storage.types()[imm.type.idx].tag;
            if (ir::is_integer_type(tag)) {
              field = static_cast<u32>(imm.as_u64_integer(tag));
              have_field = true;
            }
          }
        }
        if (!have_field || field != 0) {
          flow[instr.dst.idx].clear();
          break;
        }
        if (home[src] != NO_ROOT) {
          home[instr.dst.idx] = home[src];
          copy_path(instr.dst.idx, src);
        } else {
          for (u32 loan : flow[src]) {
            if (loan < loans.size() && loans[loan].place.root != NO_ROOT) {
              home[instr.dst.idx] = loans[loan].place.root;
              set_row(instr.dst.idx, loans[loan].place);
              break;
            }
          }
        }
        flow[instr.dst.idx] = flow[src];
        break;
      }
      case ir::Opcode::GetElementPtr: {
        u32 base = NO_ROOT;
        if (!operand_reg(0, base) || !instr.dst.is_valid()) {
          break;
        }
        if (home[base] == NO_ROOT) {
          break;
        }
        home[instr.dst.idx] = home[base];
        copy_path(instr.dst.idx, base);
        for (u32 offset = 2; offset < instr.operands.size(); ++offset) {
          const ir::Operand& index =
              storage.operands()[instr.operands.head() + offset];
          if (!index.is<ir::ImmutableIdx>()) {
            continue;
          }
          const ir::Immutable& imm = storage.immutables()[index.as_immutable()];
          push_path_step(
              instr.dst.idx,
              static_cast<u32>(imm.as_u64_integer(tag_of(imm.type))));
        }
        flow[instr.dst.idx] = flow[base];
        break;
      }
      case ir::Opcode::EnumFieldAddr: {
        // A variant payload's address names a subplace of the enum the
        // way a GEP names one of a struct: the variant and the field
        // say which payload the loan flows into, so the address keeps
        // the place and its loans (an `Option<&T>` hands a pointer
        // back only while its payload's loan is alive).
        u32 base = NO_ROOT;
        if (!operand_reg(0, base) || !instr.dst.is_valid()) {
          break;
        }
        if (home[base] == NO_ROOT) {
          break;
        }
        home[instr.dst.idx] = home[base];
        copy_path(instr.dst.idx, base);
        for (u32 offset = 1; offset < instr.operands.size(); ++offset) {
          const ir::Operand& index =
              storage.operands()[instr.operands.head() + offset];
          if (!index.is<ir::ImmutableIdx>()) {
            continue;
          }
          const ir::Immutable& imm = storage.immutables()[index.as_immutable()];
          push_path_step(
              instr.dst.idx,
              static_cast<u32>(imm.as_u64_integer(tag_of(imm.type))));
        }
        flow[instr.dst.idx] = flow[base];
        break;
      }
      case ir::Opcode::ElemOffset:
      case ir::Opcode::TypeCast: {
        // `elem_ref` and `uninit_ref` lower to these, and a container's
        // read accessors are built from them, so a loan has to survive
        // both or `Vec::at` hands back a pointer the checker never saw.
        // An element offset names a place inside the buffer it walks
        // from, and the wrapper is representation-transparent, so both
        // keep the source's place and its loans. A raw pointer
        // destination is the exception: raw pointers are outside the
        // region system (ADR-0050), so the address travels but the
        // loan dies at the cast.
        u32 src = NO_ROOT;
        if (!operand_reg(0, src) || !instr.dst.is_valid()) {
          break;
        }
        if (ir::is_raw_ptr_type(tag_of(storage.registers()[instr.dst].type))) {
          break;
        }
        if (home[src] == NO_ROOT) {
          break;
        }
        home[instr.dst.idx] = home[src];
        copy_path(instr.dst.idx, src);
        if (instr.op == ir::Opcode::ElemOffset) {
          // The index is a runtime value, so it is a step that names no
          // field: any element of the buffer, which is what lets a store
          // to the buffer itself conflict with a loan into one element.
          push_path_step(instr.dst.idx, ELEMENT_STEP);
        }
        flow[instr.dst.idx] = flow[src];
        break;
      }
      case ir::Opcode::Move: {
        u32 src = NO_ROOT;
        if (operand_reg(0, src) && instr.dst.is_valid()) {
          flow[instr.dst.idx] = flow[src];
        }
        break;
      }
      case ir::Opcode::Borrow: {
        u32 place_reg = NO_ROOT;
        if (!operand_reg(0, place_reg) || !instr.dst.is_valid()) {
          break;
        }
        Place place;
        if (place_of(storage.operands()[instr.operands.head()], place)) {
          const bool exclusive = tag_of(storage.registers()[instr.dst].type) ==
                                 ir::TypeTag::MutRef;
          // A reborrow stands behind the loans already reaching the place
          // it borrows: `&b.n` on a `&Box` is derived from the loan the
          // caller made on `b`. Carrying those forward is what lets a
          // return name the parameter it reborrows from, so
          // `fn get(b: &Box) -> &i32 { ret &b.n }` reaches the caller as a
          // loan on the caller's argument rather than a bare pointer.
          flow[instr.dst.idx] = flow[place_reg];
          flow[instr.dst.idx].push_back(static_cast<u32>(loans.size()));
          // A `str` local is a view: a borrow of it names the buffer
          // the view reads through, and the slot that holds the view is
          // the other place the same loan reaches. Both are stated, so
          // a later borrow of the slot and one of the buffer compare as
          // the overlapping loans they are. The slot stands for the
          // view, not for the two words a copy of it owns, which is why
          // copying the view out is still a plain copy.
          if (place.len == 0 && place.root < storage.registers().size() &&
              tag_of(storage.registers()[place.root].type) ==
                  ir::TypeTag::Str) {
            Place slot;
            slot.root = place.root;
            loans.push_back({instr.dst.idx, slot, exclusive, pos});
          }
          loans.push_back({instr.dst.idx, place, exclusive, pos});
        }
        break;
      }
      case ir::Opcode::Store: {
        u32 value = NO_ROOT;
        u32 addr = NO_ROOT;
        if (!operand_reg(0, value) || !operand_reg(1, addr)) {
          break;
        }
        auto union_loan = [&](u32 target, u32 loan) {
          if (target >= flow.size()) {
            return;
          }
          for (u32 prior : flow[target]) {
            if (prior == loan) {
              return;
            }
          }
          flow[target].push_back(loan);
        };
        for (u32 loan : flow[value]) {
          union_loan(addr, loan);
        }
        // Spills into aggregates keep the holder live: field stores
        // also join the place root, so whole-aggregate uses observe
        // field loans when computing expiry.
        Place stored;
        if (place_of(storage.operands()[instr.operands.head() + 1], stored)) {
          for (u32 loan : flow[value]) {
            union_loan(stored.root, loan);
          }
        }
        break;
      }
      case ir::Opcode::Call: {
        if (instr.operands.empty()) {
          break;
        }
        const ir::Operand& callee = storage.operands()[instr.operands.head()];
        if (!instr.dst.is_valid()) {
          break;
        }
        if (!callee.is<ir::FunctionIdx>() &&
            !callee.is<ir::ExternalFunctionIdx>()) {
          // A call through a function value has no summary to
          // reify: the callee is unknown, so the result
          // conservatively carries the loans of every argument
          // and of the value itself. A result that cannot carry
          // loans tracks nothing, the way a call without a
          // destination does.
          if (callee.is<ir::RegisterIdx>()) {
            const ir::TypeIdx dst_type =
                storage.registers()[instr.dst.idx].type;
            if (carries_reference(dst_type)) {
              const bool result_exclusive =
                  storage.types()[dst_type.idx].tag == ir::TypeTag::MutRef;
              bool placed = false;
              // Fresh loans never repeat, so no dedup scan: each
              // source loan makes exactly one.
              const auto carry = [&](u32 reg) {
                if (reg >= flow.size()) {
                  return;
                }
                for (u32 loan : flow[reg]) {
                  if (loan >= loans.size()) {
                    continue;
                  }
                  loans.push_back({instr.dst.idx, loans[loan].place,
                                   loans[loan].exclusive || result_exclusive,
                                   pos});
                  flow[instr.dst.idx].push_back(
                      static_cast<u32>(loans.size() - 1));
                  if (!placed) {
                    home[instr.dst.idx] = loans[loan].place.root;
                    set_row(instr.dst.idx, loans[loan].place);
                    placed = true;
                  }
                }
              };
              for (u32 offset = 1; offset < instr.operands.size(); ++offset) {
                const ir::Operand& arg =
                    storage.operands()[instr.operands.head() + offset];
                if (!arg.is<ir::RegisterIdx>()) {
                  continue;
                }
                carry(arg.as_register().idx);
              }
              carry(callee.as_register().idx);
            }
          }
          break;
        }
        // External calls have no summary, so their results carry no
        // caller loans.
        if (!callee.is<ir::FunctionIdx>()) {
          break;
        }
        const ir::FunctionIdx target = callee.as_function();
        if (target.idx >= summaries.size()) {
          break;
        }
        const bool result_exclusive =
            tag_of(storage.registers()[instr.dst.idx].type) ==
            ir::TypeTag::MutRef;
        auto union_flow = [&](u32 dst, u32 loan) {
          for (u32 prior : flow[dst]) {
            if (prior == loan) {
              return;
            }
          }
          flow[dst].push_back(loan);
        };
        // Project each entry onto its argument: the result carries a
        // loan on the argument's referent extended by the recorded
        // path, so a loan into one field does not cover the whole
        // argument. An entry with an empty path keeps the argument's
        // own loans below. A projected place is exclusive when the
        // entry is or when the result itself is an exclusive
        // reference: returning an exclusive reference derived from a
        // shared one is already rejected where it is taken, so a
        // well-formed callee never records it.
        struct Projected {
          u32 param = NO_ROOT;
          Place place;
        };
        std::vector<Projected> projected;
        for (const SummaryEntry& entry : summaries[target.idx]) {
          if (entry.path.empty()) {
            continue;
          }
          if (entry.param + 1 >= instr.operands.size()) {
            break;
          }
          const ir::Operand& arg =
              storage.operands()[instr.operands.head() + entry.param + 1];
          if (!arg.is<ir::RegisterIdx>() ||
              arg.as_register().idx >= flow.size()) {
            continue;
          }
          Place referent;
          for (u32 loan : flow[arg.as_register().idx]) {
            if (loan < loans.size() && loans[loan].place.root != NO_ROOT) {
              referent = loans[loan].place;
              break;
            }
          }
          if (referent.root == NO_ROOT) {
            continue;
          }
          const Place place = extend_place(referent, entry.path);
          loans.push_back(
              {instr.dst.idx, place, entry.exclusive || result_exclusive, pos});
          union_flow(instr.dst.idx, static_cast<u32>(loans.size() - 1));
          projected.push_back({entry.param, place});
        }
        for (const SummaryEntry& entry : summaries[target.idx]) {
          if (entry.param + 1 >= instr.operands.size()) {
            break;
          }
          const ir::Operand& arg =
              storage.operands()[instr.operands.head() + entry.param + 1];
          if (!arg.is<ir::RegisterIdx>() ||
              arg.as_register().idx >= flow.size()) {
            continue;
          }
          for (u32 loan : flow[arg.as_register().idx]) {
            if (loan >= loans.size()) {
              continue;
            }
            // Summary tokens always propagate: a return through this
            // call reaches the caller's caller through them. A real
            // loan strictly inside a projected place is superseded by
            // it and does not propagate; anything else keeps today's
            // behavior.
            if (loans[loan].param == NO_ROOT && !entry.path.empty()) {
              bool covered = false;
              for (const Projected& proj : projected) {
                if (proj.param != entry.param ||
                    proj.place.root != loans[loan].place.root) {
                  continue;
                }
                const Place& wide = loans[loan].place;
                const Place& narrow = proj.place;
                if (wide.len < narrow.len) {
                  bool prefix = true;
                  for (u32 i = 0; i < wide.len; ++i) {
                    if (!steps_match(path_steps_[wide.head + i],
                                     path_steps_[narrow.head + i])) {
                      prefix = false;
                      break;
                    }
                  }
                  if (prefix) {
                    covered = true;
                    break;
                  }
                }
              }
              if (covered) {
                continue;
              }
            }
            union_flow(instr.dst.idx, loan);
          }
        }
        if (!projected.empty() && instr.dst.idx < home.size()) {
          home[instr.dst.idx] = projected.front().place.root;
          set_row(instr.dst.idx, projected.front().place);
        }
        break;
      }
      default: break;
    }
  }

  // A loan stays live through every use of every value derived from it
  // (moves, loads, and aggregate holders propagate the identity
  // forward), so sequential borrows of one place stop being live while
  // interleaved ones stay so. Liveness is a backward dataflow over the
  // CFG: a loan is live where a value carrying it is read, and live
  // wherever a successor is live. Sibling branches therefore differ,
  // which is what lets a return taken where the loan is already dead
  // end the borrowed value without reporting a conflict.
  void compute_liveness(const ir::Function& fn,
                        const std::vector<ir::BlockIdx>& order) {
    // A loan is last read at the last instruction that reads any
    // register carrying it, in the block that instruction is in.
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
      u32* const row =
          loan_last_use.data() + static_cast<usize>(bidx.idx) * loan_stride;
      for (ir::InstructionIdx iidx : block.instrs) {
        const ir::Instruction& instr = instr_at(iidx);
        for (u32 offset = 0; offset < instr.operands.size(); ++offset) {
          const ir::Operand& operand =
              storage.operands()[instr.operands.head() + offset];
          if (!operand.is<ir::RegisterIdx>()) {
            continue;
          }
          const u32 reg = operand.as_register().idx;
          if (reg >= flow.size()) {
            continue;
          }
          for (u32 loan : flow[reg]) {
            if (loan < loans.size() && iidx.idx > row[loan]) {
              row[loan] = iidx.idx;
            }
          }
        }
      }
    }
    // Backward to a fixed point: a block is live out wherever any
    // successor is live in, and live in wherever it is read on a path
    // that reaches one of them. The CFG rows this reads are the ones
    // build_preds filled before the may-move fixpoint, which runs first.
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
      if (block.instrs.empty()) {
        continue;
      }
      block_first[bidx.idx] = block.instrs.head().idx;
      block_last[bidx.idx] = block.instrs.head().idx + block.instrs.size() - 1;
    }
    const usize cap = order.size() * 10 + 10;
    // The two sets are built in scratch that each iteration reuses, and
    // assigned into the rows rather than moved out of a fresh vector: a
    // row keeps its capacity and the sweep allocates nothing.
    for (usize iter = 0; iter < cap; ++iter) {
      bool changed = false;
      for (ir::BlockIdx bidx : order) {
        live_out_scratch_.clear();
        if (++live_stamp == 0) {
          std::fill(live_mark.begin(), live_mark.end(), 0);
          live_stamp = 1;
        }
        const u32 stamp = live_stamp;
        const u32 succ_begin = succ_head[bidx.idx];
        const u32 succ_end = succ_begin + succ_len[bidx.idx];
        for (u32 si = succ_begin; si < succ_end; ++si) {
          const ir::BlockIdx succ = succ_flat[si];
          for (u32 loan : live_in[succ.idx]) {
            if (live_mark[loan] == stamp) {
              continue;
            }
            live_mark[loan] = stamp;
            live_out_scratch_.push_back(loan);
          }
        }
        if (live_out_scratch_ != live_out[bidx.idx]) {
          live_out[bidx.idx] = live_out_scratch_;
          changed = true;
        }
        // A loan is live on entry when it is read in this block or live
        // on exit, and it was not born here: a loan born in the block
        // starts at its own birth, so it is never live before it. That
        // exclusion is what stops a loan made in a loop body from
        // wrapping around the back edge and being live on the next
        // entry, where it would outlive the iteration that made it.
        // The marks hold this block's live-out set, and index order keeps
        // the set free of repeats.
        live_in_scratch_.clear();
        const u32 first = block_first[bidx.idx];
        const u32 last = block_last[bidx.idx];
        const u32* const last_use =
            loan_last_use.data() + static_cast<usize>(bidx.idx) * loan_stride;
        for (u32 loan = 0; loan < loans.size(); ++loan) {
          const u32 birth = loans[loan].birth;
          if (birth >= first && birth <= last) {
            continue;
          }
          if (last_use[loan] == 0 && live_mark[loan] != stamp) {
            continue;
          }
          live_in_scratch_.push_back(loan);
        }
        if (live_in_scratch_ != live_in[bidx.idx]) {
          live_in[bidx.idx] = live_in_scratch_;
          changed = true;
        }
      }
      if (!changed) {
        return;
      }
    }
    DCHECK(false);
  }

  bool live_at(u32 loan, ir::BlockIdx bidx, u32 pos) const {
    if (loan >= loans.size() || loans[loan].birth > pos) {
      return false;
    }
    for (u32 prior : live_out[bidx.idx]) {
      if (prior == loan) {
        return true;
      }
    }
    return loan < loans.size() &&
           loan_last_use[static_cast<usize>(bidx.idx) * loan_stride + loan] >=
               pos;
  }

  void check_place_use(const std::vector<Place>& moved,
                       const Place& place,
                       diag::Span span,
                       std::string_view action) {
    for (const Place& gone : moved) {
      if (overlaps(gone, place, path_steps_)) {
        const u32 index = bag.emit<i18n::Key::BorrowUseAfterMove>(
            DiagCode::UseAfterMove, span, addr_name(place.root), action);
        (void)index;
        return;
      }
    }
  }

  bool same_place(const Place& a, const Place& b) const {
    if (a.root != b.root || a.len != b.len) {
      return false;
    }
    const std::span<const u32> steps = steps_of(a);
    return std::equal(steps.begin(), steps.end(), steps_of(b).begin());
  }

  bool same_state(const std::vector<Place>& a,
                  const std::vector<Place>& b) const {
    if (a.size() != b.size()) {
      return false;
    }
    for (const Place& place : a) {
      bool found = false;
      for (const Place& other : b) {
        if (same_place(place, other)) {
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    return true;
  }

  void add_moved(std::vector<Place>& state, const Place& place) const {
    for (const Place& prior : state) {
      if (same_place(prior, place)) {
        return;
      }
    }
    state.push_back(place);
  }

  void kill_moved(std::vector<Place>& state, const Place& place) const {
    for (usize i = 0; i < state.size();) {
      if (overlaps(state[i], place, path_steps_)) {
        state[i] = state.back();
        state.pop_back();
      } else {
        ++i;
      }
    }
  }

  // Successor blocks of a terminator; Ret/Unreachable end the path.
  void successors(ir::BlockIdx block, std::vector<ir::BlockIdx>& out) const {
    const ir::Block& blk = storage.blocks()[block];
    if (blk.instrs.empty()) {
      return;
    }
    const ir::Instruction& term = instr_at(
        ir::InstructionIdx(blk.instrs.head().idx + blk.instrs.size() - 1));
    auto push_target = [&](ir::OperandIdx oidx) {
      const ir::Operand& operand = storage.operands()[oidx];
      if (operand.is<ir::BlockIdx>()) {
        out.push_back(operand.as_block());
      }
    };
    switch (term.op) {
      case ir::Opcode::Br:
        if (!term.operands.empty()) {
          push_target(term.operands.head());
        }
        break;
      case ir::Opcode::CondBr:
        if (term.operands.size() == 3) {
          push_target(term.operands.head() + 1);
          push_target(term.operands.head() + 2);
        }
        break;
      case ir::Opcode::Switch:
        for (u32 i = 1; i < term.operands.size(); ++i) {
          push_target(term.operands.head() + i);
        }
        break;
      default: break;
    }
  }

  // Reverse post-order over reachable blocks; unreachable blocks
  // append in layout order so every block still gets a state.
  void reverse_post_order(const ir::Function& fn,
                          std::vector<ir::BlockIdx>& order) {
    // The walk marks blocks with this call's stamp rather than clearing a
    // table sized to the whole program, which would charge every function
    // for every block.
    if (++walk_stamp == 0) {
      std::fill(walk_mark.begin(), walk_mark.end(), 0);
      walk_stamp = 1;
    }
    const u32 stamp = walk_stamp;
    std::vector<ir::BlockIdx>& stack = stack_scratch_;
    std::vector<ir::BlockIdx>& post = post_scratch_;
    std::vector<ir::BlockIdx>& succs = succs_scratch_;
    stack.clear();
    post.clear();
    stack.push_back(fn.blocks.head());
    walk_mark[fn.blocks.head().idx] = stamp;
    while (!stack.empty()) {
      const ir::BlockIdx top = stack.back();
      succs.clear();
      successors(top, succs);
      bool descended = false;
      for (ir::BlockIdx succ : succs) {
        if (succ.idx < walk_mark.size() && walk_mark[succ.idx] != stamp) {
          walk_mark[succ.idx] = stamp;
          stack.push_back(succ);
          descended = true;
        }
      }
      if (!descended) {
        post.push_back(top);
        stack.pop_back();
      }
    }
    for (usize i = post.size(); i-- > 0;) {
      order.push_back(post[i]);
    }
    for (ir::BlockIdx bidx : fn.blocks) {
      if (bidx.idx < walk_mark.size() && walk_mark[bidx.idx] != stamp) {
        order.push_back(bidx);
      }
    }
  }

  // Moves a block state forward without diagnostics.
  void transfer(ir::BlockIdx bidx,
                const std::vector<Place>& in,
                std::vector<Place>& out) {
    out = in;
    const ir::Block& block = storage.blocks()[bidx];
    for (ir::InstructionIdx iidx : block.instrs) {
      const ir::Instruction& instr = instr_at(iidx);
      if (instr.op == ir::Opcode::Move && !instr.operands.empty()) {
        Place place;
        if (place_of(storage.operands()[instr.operands.head()], place)) {
          add_moved(out, place);
        }
      } else if (instr.op == ir::Opcode::Drop && !instr.operands.empty()) {
        // A placed end leaves the value owned at the edge: the guard
        // behind a maybe has already decided, so what follows owns it.
        Place place;
        if (place_of(storage.operands()[instr.operands.head()], place)) {
          kill_moved(out, place);
        }
      } else if (instr.op == ir::Opcode::Store && instr.operands.size() == 2) {
        Place place;
        if (place_of(storage.operands()[instr.operands.head() + 1], place)) {
          kill_moved(out, place);
        }
      }
    }
  }

  // May-move fixpoint: back-edges carry loop moves to the next
  // iteration (bounded; the place universe is finite).
  void compute_moved(const std::vector<ir::BlockIdx>& order) {
    const usize cap = order.size() * 10 + 10;
    for (usize iter = 0; iter < cap; ++iter) {
      bool changed = false;
      for (ir::BlockIdx bidx : order) {
        move_in_scratch_.clear();
        for (ir::BlockIdx pred : preds[bidx.idx]) {
          for (const Place& place : moved_out[pred.idx]) {
            add_moved(move_in_scratch_, place);
          }
        }
        transfer(bidx, move_in_scratch_, move_out_scratch_);
        // IN is refreshed every visit for the check pass to read;
        // successors are fed from OUT, which alone drives convergence.
        // The rows take the scratch by swap: a state is copied per place of
        // it, and a copy names the same windows either way.
        moved_in[bidx.idx].swap(move_in_scratch_);
        if (!same_state(move_out_scratch_, moved_out[bidx.idx])) {
          moved_out[bidx.idx].swap(move_out_scratch_);
          changed = true;
        }
      }
      if (!changed) {
        return;
      }
    }
    DCHECK(false);
  }

  // Two-phase borrows: which exclusive receiver borrows serve one call.
  // A `Borrow` whose destination is used exactly once, as the receiver
  // operand of a direct call, is a reservation: the call activates it,
  // and the argument evaluation between the two is the window
  // ADR-0012 opens for reads of the place.
  void collect_reserved(const ir::Function& fn) {
    // Both scratch vectors are sized with the package and cleared per
    // register by reset_function, so counting a function touches only what
    // the function names rather than zeroing every register it cannot.
    const usize registers = use_count_scratch_.size();
    for (ir::BlockIdx bidx : fn.blocks) {
      for (ir::InstructionIdx iidx : storage.blocks()[bidx].instrs) {
        const ir::Instruction& instr = instr_at(iidx);
        const bool direct_call =
            instr.op == ir::Opcode::Call && !instr.operands.empty() &&
            storage.operands()[instr.operands.head()].is<ir::FunctionIdx>();
        for (u32 offset = 0; offset < instr.operands.size(); ++offset) {
          const ir::Operand& operand =
              storage.operands()[instr.operands.head() + offset];
          if (!operand.is<ir::RegisterIdx>()) {
            continue;
          }
          const u32 reg = operand.as_register().idx;
          if (reg >= registers) {
            continue;
          }
          ++use_count_scratch_[reg];
          if (direct_call && offset == 1) {
            receiver_call_scratch_[reg] = 1;
          }
        }
      }
    }
    for (ir::BlockIdx bidx : fn.blocks) {
      for (ir::InstructionIdx iidx : storage.blocks()[bidx].instrs) {
        const ir::Instruction& instr = instr_at(iidx);
        if (instr.op != ir::Opcode::Borrow || !instr.dst.is_valid() ||
            instr.dst.idx >= reserved_receiver.size()) {
          continue;
        }
        reserved_receiver[instr.dst.idx] =
            use_count_scratch_[instr.dst.idx] == 1 &&
                    receiver_call_scratch_[instr.dst.idx] != 0 &&
                    tag_of(storage.registers()[instr.dst].type) ==
                        ir::TypeTag::MutRef
                ? 1
                : 0;
      }
    }
  }

  void check_function(ir::FunctionIdx fidx) {
    const ir::Function& fn = storage.functions()[fidx];
    collect_reserved(fn);
    forward(fidx);
    block_order_.clear();
    reverse_post_order(fn, block_order_);
    reset_blocks(block_order_);
    // Both fixpoints walk the same CFG of the same function, so its rows are
    // built once here.
    build_preds(fn, block_order_);
    compute_moved(block_order_);
    compute_liveness(fn, block_order_);
    for (ir::BlockIdx bidx : block_order_) {
      // The entry state is copied rather than moved out of its row: the row
      // keeps its buffer, and the copy lands in the scratch the previous
      // block left, so the pass allocates nothing per block. A state is a
      // handful of places, each of them a window on the shared step arena.
      state_scratch_ = moved_in[bidx.idx];
      const ir::Block& block = storage.blocks()[bidx];
      for (ir::InstructionIdx iidx : block.instrs) {
        check_instr(fn, bidx, iidx, state_scratch_);
      }
    }
  }

  void check_instr(const ir::Function& fn,
                   ir::BlockIdx bidx,
                   ir::InstructionIdx iidx,
                   std::vector<Place>& moved) {
    const ir::Instruction& instr = instr_at(iidx);
    const u32 pos = iidx.idx;
    const diag::Span span = iidx.idx < lowered.instr_spans.size()
                                ? lowered.instr_spans[iidx.idx]
                                : diag::Span{};
    auto operand_reg = [&](u32 offset, u32& reg) {
      if (offset >= instr.operands.size()) {
        return false;
      }
      const ir::Operand& operand =
          storage.operands()[instr.operands.head() + offset];
      if (!operand.is<ir::RegisterIdx>()) {
        return false;
      }
      reg = operand.as_register().idx;
      return true;
    };
    auto operand_place = [&](u32 offset, Place& place) {
      if (offset >= instr.operands.size()) {
        return false;
      }
      return place_of(storage.operands()[instr.operands.head() + offset],
                      place);
    };
    // Whether a loan is one the value at this operand already carries,
    // so an access through it is access the loan authorized.
    auto carries_loan = [&](u32 reg, u32 id) {
      if (reg >= flow.size()) {
        return false;
      }
      for (u32 held : flow[reg]) {
        if (held == id) {
          return true;
        }
      }
      return false;
    };
    switch (instr.op) {
      case ir::Opcode::EnumTagLoad:
      case ir::Opcode::Load: {
        // Reading a place is a use, so a read of a moved place is a
        // use-after-move.
        Place place;
        u32 addr_reg = NO_ROOT;
        if (operand_place(0, place)) {
          check_place_use(moved, place, span, "read");
          // An exclusive loan freezes the place it covers, reading
          // included. Reading through a loan is exempt, since the
          // address carries the loan it was taken by; reading the same
          // place by another route is the conflict.
          for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
            const Loan& loan = loans[id];
            if (loan.param != NO_ROOT || !loan.exclusive) {
              continue;
            }
            operand_reg(0, addr_reg);
            if (carries_loan(addr_reg, id)) {
              continue;
            }
            if (!live_at(id, bidx, pos) ||
                !overlaps(loan.place, place, path_steps_)) {
              continue;
            }
            // A reserved receiver's window admits a read of data: the
            // call that activates the reservation re-checks the place,
            // so a read that outlives the call still reports. Reading a
            // reference out is not data, so it stays a conflict.
            if (instr.dst.is_valid() && loan.reg < reserved_receiver.size() &&
                reserved_receiver[loan.reg] != 0) {
              const ir::TypeTag loaded =
                  tag_of(storage.registers()[instr.dst.idx].type);
              if (loaded != ir::TypeTag::Ref && loaded != ir::TypeTag::MutRef) {
                continue;
              }
            }
            const u32 index = bag.emit<i18n::Key::BorrowConflict>(
                DiagCode::Conflict, span, addr_name(place.root));
            (void)index;
            break;
          }
        }
        break;
      }
      case ir::Opcode::Move: {
        Place place;
        if (!operand_place(0, place)) {
          break;
        }
        // A place reached through a reference is borrowed content:
        // moving out of it would leave the referent owned twice, by
        // whoever put the reference in the environment and by
        // whoever owns the place it reads. A borrowed capture names
        // exactly such a place, through its environment.
        bool borrowed = false;
        for (u32 step : steps_of(place)) {
          if (step == DEREF_STEP) {
            borrowed = true;
            break;
          }
        }
        if (borrowed) {
          const u32 index = bag.emit<i18n::Key::BorrowMoveOutOfBorrow>(
              DiagCode::MoveOutOfBorrow, span, addr_name(place.root));
          (void)index;
          break;
        }
        // ADR-0085: an unmovable value is never moved. The root's type
        // decides, because a part of an unmovable value is reached
        // through borrows only.
        const ir::TypeIdx root_type = storage.registers()[place.root].type;
        if (!ir::is_movable_type(storage.state(), root_type,
                                 lowered.not_movable, [this](ir::TypeIdx idx) {
                                   const auto found =
                                       lowered.type_origins.find(idx.idx);
                                   return found == lowered.type_origins.end()
                                              ? idx
                                              : ir::TypeIdx(found->second);
                                 })) {
          const u32 index = bag.emit<i18n::Key::BorrowUnmovableMoved>(
              DiagCode::UnmovableMoved, span, addr_name(place.root));
          (void)index;
          break;
        }
        check_place_use(moved, place, span, "move");
        for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
          const Loan& loan = loans[id];
          if (loan.param != NO_ROOT) {
            continue;
          }
          if (!live_at(id, bidx, pos) ||
              !overlaps(loan.place, place, path_steps_)) {
            continue;
          }
          const u32 index = bag.emit<i18n::Key::BorrowMoveInvalidatesBorrow>(
              DiagCode::UseAfterMove, span, addr_name(place.root));
          (void)index;
          break;
        }
        add_moved(moved, place);
        break;
      }
      case ir::Opcode::Drop: {
        // A placed end is authorized: the compiler put it where the
        // value is still owned on this path (a guard decides a maybe at
        // run time), so the end's own move is not a use after one.
        Place place;
        if (operand_place(0, place)) {
          kill_moved(moved, place);
        }
        break;
      }
      case ir::Opcode::Borrow: {
        Place place;
        if (!operand_place(0, place)) {
          break;
        }
        check_place_use(moved, place, span, "borrow");
        const bool exclusive =
            instr.dst.is_valid() &&
            tag_of(storage.registers()[instr.dst].type) == ir::TypeTag::MutRef;
        // A reborrow is not in conflict with the loan it was taken
        // from: it stands behind that loan and ends with it, which is
        // what rule 1 means by being valid exactly as long as the
        // enclosing borrow. It may not be *more* exclusive than what it
        // derives from, so an exclusive reborrow of a shared loan is
        // still a conflict.
        auto derived_from = [&](u32 id) {
          for (u32 held : flow[instr.dst.idx]) {
            if (held == id) {
              return true;
            }
          }
          return false;
        };
        for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
          const Loan& loan = loans[id];
          if (loan.reg == instr.dst.idx || loan.param != NO_ROOT) {
            continue;
          }
          if (derived_from(id) && !(exclusive && !loan.exclusive)) {
            continue;
          }
          if (!live_at(id, bidx, pos) ||
              !overlaps(loan.place, place, path_steps_)) {
            continue;
          }
          // A shared borrow during a reserved receiver's window is the
          // read the reservation admits; the activation at the call is
          // what still refuses one that outlives it.
          if (!exclusive && loan.exclusive &&
              loan.reg < reserved_receiver.size() &&
              reserved_receiver[loan.reg] != 0) {
            continue;
          }
          if (exclusive || loan.exclusive) {
            const u32 index = bag.emit<i18n::Key::BorrowConflict>(
                DiagCode::Conflict, span, addr_name(place.root));
            (void)index;
            break;
          }
        }
        break;
      }
      case ir::Opcode::Store: {
        Place place;
        u32 target = NO_ROOT;
        if (operand_reg(1, target) && operand_place(1, place)) {
          // The address of a moved place is not a value that may be
          // passed on. A closure call hands its environment over as the
          // closure variable's own address, so a moved-from closure
          // still reaches the storage its move gave away. A register
          // that names a place without a deref step is that address;
          // one loaded from a reference names the referent, which a
          // move of the slot does not take.
          u32 handed = NO_ROOT;
          Place handed_place;
          if (operand_reg(0, handed) && handed < path_len.size()) {
            const std::span<const u32> steps = steps_of(handed);
            if (std::find(steps.begin(), steps.end(), DEREF_STEP) ==
                    steps.end() &&
                operand_place(0, handed_place)) {
              check_place_use(moved, handed_place, span, "read");
            }
          }
          kill_moved(moved, place);
          // Writing through a loan is what the loan is for, so a store
          // is exempt from the loans its own address carries. Every
          // other loan reaching the place still forbids it, which is
          // what makes a write through one reference a conflict with a
          // loan taken through another.
          for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
            const Loan& loan = loans[id];
            if (loan.param != NO_ROOT) {
              continue;
            }
            if (carries_loan(target, id)) {
              continue;
            }
            if (!live_at(id, bidx, pos) ||
                !overlaps(loan.place, place, path_steps_)) {
              continue;
            }
            const u32 index = bag.emit<i18n::Key::BorrowAssignWhileBorrowed>(
                DiagCode::AssignBorrowed, span, addr_name(place.root));
            (void)index;
            break;
          }
        }
        break;
      }
      case ir::Opcode::EnumTagSet: {
        // Setting a variant reinitializes the enum place: what was
        // there ends, and the write answers to every loan a store to
        // the place would.
        Place place;
        u32 target = NO_ROOT;
        if (operand_reg(0, target) && operand_place(0, place)) {
          kill_moved(moved, place);
          for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
            const Loan& loan = loans[id];
            if (loan.param != NO_ROOT) {
              continue;
            }
            if (carries_loan(target, id)) {
              continue;
            }
            if (!live_at(id, bidx, pos) ||
                !overlaps(loan.place, place, path_steps_)) {
              continue;
            }
            const u32 index = bag.emit<i18n::Key::BorrowAssignWhileBorrowed>(
                DiagCode::AssignBorrowed, span, addr_name(place.root));
            (void)index;
            break;
          }
        }
        break;
      }
      case ir::Opcode::Ret: {
        if (instr.operands.empty()) {
          break;
        }
        const ir::Operand& value = storage.operands()[instr.operands.head()];
        if (!value.is<ir::RegisterIdx>() ||
            value.as_register().idx >= flow.size()) {
          break;
        }
        for (u32 loan : flow[value.as_register().idx]) {
          if (loan >= loans.size() || loans[loan].param != NO_ROOT) {
            continue;
          }
          const u32 root = loans[loan].place.root;
          // A `str` local is a view, not storage: its two words point at
          // a buffer someone else owns, so handing the view back does
          // not hand back the local. A `String` owns, so a loan rooted
          // there still escapes. The exemption covers the view's value,
          // which reaches the return through a copy: the loan sits on
          // the register the copy read, not on the returned one. A loan
          // whose own register is the returned one is the address the
          // borrow took of the slot, and that address is the frame's.
          if (root < storage.registers().size() &&
              tag_of(storage.registers()[root].type) == ir::TypeTag::Str &&
              loans[loan].reg != value.as_register().idx) {
            continue;
          }
          if (!is_param_root(root, fn)) {
            const u32 index = bag.emit<i18n::Key::BorrowReturnsLocalReference>(
                DiagCode::Escape, span, addr_name(root));
            (void)index;
            break;
          }
        }
        break;
      }
      case ir::Opcode::Call: {
        // The reservation activates: the callee sees the exclusive
        // borrow, so any other loan still live on the place it covers
        // conflicts. Reads that ended in the argument evaluation are
        // the window two-phase borrows admit (ADR-0012).
        if (instr.operands.size() < 2 ||
            !storage.operands()[instr.operands.head()].is<ir::FunctionIdx>()) {
          break;
        }
        const ir::Operand& receiver =
            storage.operands()[instr.operands.head() + 1];
        if (!receiver.is<ir::RegisterIdx>() ||
            receiver.as_register().idx >= reserved_receiver.size() ||
            reserved_receiver[receiver.as_register().idx] == 0) {
          break;
        }
        for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
          const Loan& reserved = loans[id];
          if (reserved.reg != receiver.as_register().idx ||
              reserved.param != NO_ROOT) {
            continue;
          }
          for (u32 other = 0; other < static_cast<u32>(loans.size()); ++other) {
            if (other == id) {
              continue;
            }
            const Loan& loan = loans[other];
            if (loan.param != NO_ROOT || loan.reg == reserved.reg) {
              continue;
            }
            if (!live_at(other, bidx, pos) ||
                !overlaps(loan.place, reserved.place, path_steps_)) {
              continue;
            }
            // A reborrow taken through the receiver stands behind it,
            // the way the borrow rule lets one taken through a loan.
            if (carries_loan(loan.reg, id)) {
              continue;
            }
            // The call's own projection reifies the return's borrow of
            // the receiver - it supersedes the reservation's loan in
            // the result rather than riding beside it - so it is the
            // activation's product, not a loan competing with it.
            if (instr.dst.is_valid() && loan.reg == instr.dst.idx) {
              continue;
            }
            const u32 index = bag.emit<i18n::Key::BorrowConflict>(
                DiagCode::Conflict, span, addr_name(reserved.place.root));
            (void)index;
            break;
          }
          break;
        }
        break;
      }
      default: break;
    }
  }

  // Parameter allocas of a function: entry-block stores of
  // block-parameter registers, in declaration order. Each parameter
  // owns exactly one home alloca; destructuring derives from it.
  void param_allocas(const ir::Function& fn,
                     std::vector<std::pair<u32, u32>>& out) {
    out.clear();
    const ir::Block& entry = storage.blocks()[fn.blocks.head()];
    for (ir::InstructionIdx iidx : entry.instrs) {
      const ir::Instruction& instr = instr_at(iidx);
      if (instr.op != ir::Opcode::Store || instr.operands.size() != 2) {
        continue;
      }
      const ir::Operand& value = storage.operands()[instr.operands.head()];
      const ir::Operand& target = storage.operands()[instr.operands.head() + 1];
      if (!value.is<ir::RegisterIdx>() || !target.is<ir::RegisterIdx>()) {
        continue;
      }
      u32 position = 0;
      bool is_param = false;
      for (ir::BlockParamIdx pidx : entry.block_params) {
        if (storage.block_params()[pidx].reg.idx == value.as_register().idx) {
          is_param = true;
          break;
        }
        ++position;
      }
      if (is_param) {
        out.emplace_back(target.as_register().idx, position);
      }
    }
  }

  // Recomputes one summary from current flow: the ways a returned
  // reference reaches the parameters. A loan whose place is rooted at
  // a parameter alloca names that parameter and the path from it; a
  // leading dereference steps out of the parameter slot itself, which
  // the caller's argument already embodies, so it is dropped for a
  // by-reference parameter. Summaries only grow across sweeps.
  bool update_summary(ir::FunctionIdx fidx) {
    const ir::Function& fn = storage.functions()[fidx];
    // Which parameter a root holds. Only the parameter allocas answer this,
    // so the table is the size of a parameter list rather than of the
    // register file, which is what the checker used to build once per
    // function per sweep.
    const auto param_of = [this](u32 root) -> u32 {
      for (const auto& [alloca, index] : param_homes) {
        if (alloca == root) {
          return index;
        }
      }
      return NO_ROOT;
    };
    std::vector<ir::TypeIdx> param_types;
    for (ir::TypeIdx tidx : fn.meta.param_types) {
      param_types.push_back(tidx);
    }
    std::vector<std::pair<SummaryEntry, bool>> staged;
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
      if (block.instrs.empty()) {
        continue;
      }
      const ir::Instruction& term = instr_at(ir::InstructionIdx(
          block.instrs.head().idx + block.instrs.size() - 1));
      if (term.op != ir::Opcode::Ret || term.operands.empty()) {
        continue;
      }
      const ir::Operand& value = storage.operands()[term.operands.head()];
      if (!value.is<ir::RegisterIdx>() ||
          value.as_register().idx >= flow.size()) {
        continue;
      }
      for (u32 loan : flow[value.as_register().idx]) {
        if (loan >= loans.size()) {
          continue;
        }
        const Loan& entry = loans[loan];
        u32 param = entry.param;
        std::vector<u32> relpath;
        if (param == NO_ROOT) {
          param = param_of(entry.place.root);
          if (param == NO_ROOT) {
            continue;
          }
          const std::span<const u32> steps = steps_of(entry.place);
          relpath.assign(steps.begin(), steps.end());
          if (param < param_types.size()) {
            const ir::TypeTag ptag = tag_of(param_types[param]);
            if ((ptag == ir::TypeTag::Ref || ptag == ir::TypeTag::MutRef) &&
                !relpath.empty() && relpath.front() == DEREF_STEP) {
              relpath.erase(relpath.begin());
            }
          }
        }
        SummaryEntry candidate{param, std::move(relpath), entry.exclusive};
        const bool candidate_from_token = entry.param != NO_ROOT;
        bool known = false;
        for (const auto& [prior, _] : staged) {
          if (prior.param == candidate.param && prior.path == candidate.path &&
              prior.exclusive == candidate.exclusive) {
            known = true;
            break;
          }
        }
        if (!known) {
          staged.emplace_back(std::move(candidate), candidate_from_token);
        }
      }
    }
    // A token entry names the whole parameter, so it is redundant where
    // a narrower entry names a part of the same parameter: the returned
    // place is the narrower one, and keeping the whole one would make
    // every call site cover the argument it no longer needs to.
    std::vector<SummaryEntry> next;
    for (const auto& [candidate, from_token] : staged) {
      if (from_token) {
        bool narrowed = false;
        for (const auto& [other, other_token] : staged) {
          if (!other_token && other.param == candidate.param &&
              !other.path.empty()) {
            narrowed = true;
            break;
          }
        }
        if (narrowed) {
          continue;
        }
      }
      bool known = false;
      for (const SummaryEntry& prior : next) {
        if (prior.param == candidate.param && prior.path == candidate.path &&
            prior.exclusive == candidate.exclusive) {
          known = true;
          break;
        }
      }
      if (!known) {
        next.push_back(candidate);
      }
    }
    if (next.size() == summaries[fidx.idx].size()) {
      bool same = true;
      for (const SummaryEntry& index : next) {
        bool found = false;
        for (const SummaryEntry& prior : summaries[fidx.idx]) {
          if (prior.param == index.param && prior.path == index.path &&
              prior.exclusive == index.exclusive) {
            found = true;
            break;
          }
        }
        if (!found) {
          same = false;
          break;
        }
      }
      if (same) {
        return false;
      }
    }
    summaries[fidx.idx] = std::move(next);
    return true;
  }

  // Sizes the scratch for the whole program, once. Its entries are indexed
  // by global register, so all of them have to exist, but a function clears
  // only the entries it names rather than every entry.
  void size_scratch() {
    const usize registers = storage.registers().size();
    home.assign(registers, NO_ROOT);
    // One step stands at the arena's head so every empty path reads as a
    // window on it rather than on nothing.
    path_steps_.assign(1, 0);
    path_head.assign(registers, 0);
    path_len.assign(registers, 0);
    flow_steps.clear();
    flow.assign(registers, FlowRow{});
    for (FlowRow& row : flow) {
      row.arena = &flow_steps;
    }
    reserved_receiver.assign(registers, 0);
    use_count_scratch_.assign(registers, 0);
    receiver_call_scratch_.assign(registers, 0);
    // Sized for the whole program: a type's shape does not change, so its
    // answer holds across functions.
    carry_memo_.assign(storage.types().size(), 0);
    const usize blocks = storage.blocks().size();
    live_in.assign(blocks, {});
    live_out.assign(blocks, {});
    moved_in.assign(blocks, {});
    moved_out.assign(blocks, {});
    preds.assign(blocks, {});
    succ_head.assign(blocks, 0);
    succ_len.assign(blocks, 0);
    block_first.assign(blocks, 0);
    block_last.assign(blocks, 0);
    walk_mark.assign(blocks, 0);
    // The flat successor arena and the walk's stacks are cleared per
    // function; sizing them for the whole program here keeps the clear
    // from re-growing a buffer for every function it holds.
    succ_flat.reserve(3 * blocks);
    succs_scratch_.reserve(3);
    stack_scratch_.reserve(blocks);
    post_scratch_.reserve(blocks);
    index_register_bounds(registers);
    index_addrs();
    seed_all_captures();
  }

  // The lowest and highest register a function names, as a destination or as
  // a register operand, clamped to the register table. A register past the
  // table cannot be reached through a place, so the range ends at it.
  void index_register_bounds(usize registers) {
    const usize functions = storage.functions().size();
    fn_reg_lo.assign(functions, 0);
    fn_reg_hi.assign(functions, 0);
    for (usize fidx = 0; fidx < functions; ++fidx) {
      u32 lo = static_cast<u32>(registers);
      u32 hi = 0;
      for (ir::BlockIdx bidx : storage.functions()[fidx].blocks) {
        for (ir::InstructionIdx iidx : storage.blocks()[bidx].instrs) {
          const ir::Instruction& instr = instr_at(iidx);
          if (instr.dst.is_valid()) {
            lo = std::min(lo, instr.dst.idx);
            hi = std::max(hi, instr.dst.idx + 1);
          }
          for (ir::OperandIdx oidx : instr.operands) {
            const ir::Operand& operand = storage.operands()[oidx];
            if (!operand.is<ir::RegisterIdx>()) {
              continue;
            }
            const u32 reg = operand.as_register().idx;
            lo = std::min(lo, reg);
            hi = std::max(hi, reg + 1);
          }
        }
      }
      fn_reg_hi[fidx] = std::min(hi, static_cast<u32>(registers));
      fn_reg_lo[fidx] = std::min(lo, fn_reg_hi[fidx]);
    }
  }

  // What the package named, by the address it named. A diagnostic asks for one
  // name and this asked by walking every name in the package; the captures are
  // separated out in the same walk because a function seeds them before it
  // looks at an instruction.
  void index_addrs() {
    for (const lowering::LoweredPackage::AddrInfo& entry : lowered.addr_names) {
      auto [named, inserted] = addr_by_register_.emplace(
          entry.addr.idx, NamedAddr{entry.name, entry.is_param});
      if (!inserted) {
        named->second.is_param = named->second.is_param || entry.is_param;
      }
      if (entry.is_capture && entry.addr.idx < home.size()) {
        captures_.push_back(entry.addr.idx);
      }
    }
    // Sorted and deduplicated: a function re-seeds the captures its register
    // range covers, which is a lookup rather than a walk of the package.
    std::sort(captures_.begin(), captures_.end());
    captures_.erase(std::unique(captures_.begin(), captures_.end()),
                    captures_.end());
  }

  // Clears the rows indexed by block that a function reaches, the
  // counterpart of clear_register for the CFG-shaped scratch. The list is
  // the function's block closure rather than its declared blocks, because
  // that is what the fixpoints walk. The last-use row is sized to the loans
  // the function has, which is why this waits until forward has run.
  void reset_blocks(const std::vector<ir::BlockIdx>& blocks) {
    // A row is as wide as the widest function's loan list, and a block
    // belongs to one function, so a block's row is written by that function
    // alone and zeroing it here leaves no stale entry behind.
    if (loans.size() > loan_stride) {
      loan_stride = static_cast<u32>(loans.size());
      loan_last_use.assign(storage.blocks().size() * loan_stride, 0);
    }
    for (ir::BlockIdx bidx : blocks) {
      live_in[bidx.idx].clear();
      live_out[bidx.idx].clear();
      moved_in[bidx.idx].clear();
      moved_out[bidx.idx].clear();
      preds[bidx.idx].clear();
      block_first[bidx.idx] = 0;
      block_last[bidx.idx] = 0;
      const auto row =
          loan_last_use.begin() + static_cast<usize>(bidx.idx) * loan_stride;
      std::fill_n(row, loans.size(), 0);
    }
    live_mark.assign(loans.size(), 0);
  }

  // Predecessors and successors of the blocks the fixpoints read. The walk
  // covers the function's own blocks as before, and the clearing covers every
  // block the fixpoints reach, which leaves no row another function filled
  // where one is read.
  void build_preds(const ir::Function& fn,
                   const std::vector<ir::BlockIdx>& order) {
    for (ir::BlockIdx bidx : order) {
      preds[bidx.idx].clear();
      succ_len[bidx.idx] = 0;
    }
    succ_flat.clear();
    for (ir::BlockIdx bidx : fn.blocks) {
      // successors appends, so the range belongs to one block.
      const u32 head = static_cast<u32>(succ_flat.size());
      successors(bidx, succ_flat);
      succ_head[bidx.idx] = head;
      succ_len[bidx.idx] = static_cast<u32>(succ_flat.size()) - head;
      for (usize i = head; i < succ_flat.size(); ++i) {
        const ir::BlockIdx succ = succ_flat[i];
        if (succ.idx < preds.size()) {
          preds[succ.idx].push_back(bidx);
        }
      }
    }
  }

  // Clears the scratch for one function. Every register it reads or writes is
  // named by one of its instructions, as a destination or as a register
  // operand, and the places a register stands for are reached through those,
  // so what it does not name cannot be observed through it. Clearing the whole
  // program instead charged every function for every register, which made a
  // package quadratic in its own size.
  void reset_function(ir::FunctionIdx fidx) {
    const u32 hi = fn_reg_hi[fidx.idx];
    for (u32 reg = fn_reg_lo[fidx.idx]; reg < hi; ++reg) {
      home[reg] = NO_ROOT;
      clear_path(reg);
      flow[reg].clear();
      use_count_scratch_[reg] = 0;
      receiver_call_scratch_[reg] = 0;
    }
    loans.clear();
    param_homes.clear();
  }

  void run() {
    size_scratch();
    summaries.assign(storage.functions().size(), {});
    // Bounded summary fixed-point over the call graph: one call edge
    // propagates per sweep, so a chain cannot exceed the function count
    // and the cap only fires on compiler bugs.
    const usize cap = storage.functions().size() + 1;
    bool stable = false;
    for (usize sweep = 0; sweep < cap && !stable; ++sweep) {
      stable = true;
      for (ir::FunctionIdx fidx(0); fidx.idx < storage.functions().size();
           ++fidx) {
        const ir::Function& fn = storage.functions()[fidx];
        reset_function(fidx);
        {
          PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, fn_name(fn),
                                                   "borrow-fn");
          forward(fidx);
        }
        if (update_summary(fidx)) {
          stable = false;
        }
      }
    }
    if (!stable) {
      DCHECK(false);
    }
    for (ir::FunctionIdx fidx(0); fidx.idx < storage.functions().size();
         ++fidx) {
      const ir::Function& fn = storage.functions()[fidx];
      reset_function(fidx);
      {
        PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, fn_name(fn),
                                                 "borrow-fn");
        check_function(fidx);
      }
    }
  }
};

}  // namespace

base::Result<void, diag::Reported> check_borrows(
    const lowering::LoweredPackage& lowered,
    diag::DiagBag& bag,
    ir::SymbolTable& strings,
    debug::Profiler* profiler) {
  const u32 errors = bag.error_count();
  Checker checker{lowered, *lowered.storage, bag, strings, profiler};
  checker.run();
  if (bag.error_count() != errors) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

}  // namespace borrow
