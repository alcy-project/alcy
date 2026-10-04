// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "borrow/borrow.h"

#include <string_view>
#include <utility>
#include <vector>

#include "borrow/diag_code.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/str/string_interner.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/storage.h"
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
// the other (or they are equal).
struct Place {
  u32 root = NO_ROOT;
  std::vector<u32> path;
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

bool overlaps(const Place& a, const Place& b) {
  if (a.root != b.root) {
    return false;
  }
  const usize common =
      a.path.size() < b.path.size() ? a.path.size() : b.path.size();
  for (usize i = 0; i < common; ++i) {
    if (!steps_match(a.path[i], b.path[i])) {
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

class Checker {
 public:
  Checker(const lowering::LoweredPackage& lowered,
          const ir::Storage& storage,
          diag::DiagBag& bag,
          str::StringInterner& strings,
          debug::Profiler* profiler = nullptr)
      : lowered(lowered),
        storage(storage),
        bag(bag),
        strings(strings),
        profiler(profiler) {}

  const lowering::LoweredPackage& lowered;
  const ir::Storage& storage;
  diag::DiagBag& bag;
  str::StringInterner& strings;
  // Where the trace events go, or nothing. The two sweeps - summaries,
  // then the check - run function by function on the caller's thread,
  // and each function body gets its own region under "borrow".
  debug::Profiler* profiler = nullptr;

  // Per-function scratch, indexed by global register.
  std::vector<u32> home;
  std::vector<std::vector<u32>> path;
  std::vector<std::vector<u32>> flow;
  std::vector<Loan> loans;
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
  std::vector<std::vector<u32>> loan_last_use;

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
    for (const auto& entry : lowered.addr_names) {
      if (entry.addr.idx == reg) {
        return entry.name;
      }
    }
    return "value";
  }

  bool is_param_root(u32 reg, const ir::Function& fn) const {
    for (ir::BlockParamIdx pidx :
         storage.blocks()[fn.blocks.head()].block_params) {
      if (storage.block_params()[pidx].reg.idx == reg) {
        return true;
      }
    }
    for (const auto& entry : lowered.addr_names) {
      if (entry.addr.idx == reg && entry.is_param) {
        return true;
      }
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
    place.path = path[reg];
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

  void forward(const ir::Function& fn) {
    seed_param_loans(fn);
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
        // aggregate behaves the same way, because loading
        // `H { r: &mut i32 }` copies a reference that still points into
        // the loan. A plain scalar load is a *copy* that owns nothing,
        // so the loan stops there: `x := *r` must not look like
        // returning the reference, nor keep the borrow alive past its
        // last use.
        const ir::TypeIdx loaded_ty = storage.registers()[instr.dst.idx].type;
        const ir::TypeTag loaded = tag_of(loaded_ty);
        const bool is_reborrow =
            loaded == ir::TypeTag::Ref || loaded == ir::TypeTag::MutRef;
        // A `str` is a view into someone's buffer, so a copy of one
        // keeps the buffer's loan alive exactly like a copy of a
        // reference does. Without this a view outlives the push that
        // reallocates under it: the loan dies at the copy and the
        // conflict is never reported.
        const bool carries_reference =
            is_reborrow || loaded == ir::TypeTag::Struct ||
            loaded == ir::TypeTag::Tuple || loaded == ir::TypeTag::Array ||
            loaded == ir::TypeTag::Enum || loaded == ir::TypeTag::Str ||
            loaded == ir::TypeTag::Slice || loaded == ir::TypeTag::Func;
        if (carries_reference) {
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
          u32 referent = NO_ROOT;
          std::vector<u32> referent_path;
          for (u32 loan : flow[instr.dst.idx]) {
            if (loan < loans.size() && loans[loan].place.root != NO_ROOT) {
              referent = loans[loan].place.root;
              referent_path = loans[loan].place.path;
              break;
            }
          }
          if (referent != NO_ROOT) {
            home[instr.dst.idx] = referent;
            path[instr.dst.idx] = std::move(referent_path);
          } else if (home[addr] != NO_ROOT) {
            home[instr.dst.idx] = home[addr];
            path[instr.dst.idx] = path[addr];
          } else {
            break;
          }
          path[instr.dst.idx].push_back(DEREF_STEP);
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
        bool is_slice_value = src_tag == ir::TypeTag::Slice;
        if (src_tag == ir::TypeTag::Ref || src_tag == ir::TypeTag::MutRef) {
          const ir::TypeIdx pointee =
              storage.ref_types()[storage.types()[src_type].as_ref()].pointee;
          is_slice_value = tag_of(pointee) == ir::TypeTag::Slice;
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
          path[instr.dst.idx] = path[src];
        } else {
          for (u32 loan : flow[src]) {
            if (loan < loans.size() && loans[loan].place.root != NO_ROOT) {
              home[instr.dst.idx] = loans[loan].place.root;
              path[instr.dst.idx] = loans[loan].place.path;
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
        path[instr.dst.idx] = path[base];
        for (u32 offset = 2; offset < instr.operands.size(); ++offset) {
          const ir::Operand& index =
              storage.operands()[instr.operands.head() + offset];
          if (!index.is<ir::ImmutableIdx>()) {
            continue;
          }
          const ir::Immutable& imm = storage.immutables()[index.as_immutable()];
          path[instr.dst.idx].push_back(
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
        // keep the source's place and its loans.
        u32 src = NO_ROOT;
        if (!operand_reg(0, src) || !instr.dst.is_valid()) {
          break;
        }
        if (home[src] == NO_ROOT) {
          break;
        }
        home[instr.dst.idx] = home[src];
        path[instr.dst.idx] = path[src];
        if (instr.op == ir::Opcode::ElemOffset) {
          // The index is a runtime value, so it is a step that names no
          // field: any element of the buffer, which is what lets a store
          // to the buffer itself conflict with a loan into one element.
          path[instr.dst.idx].push_back(ELEMENT_STEP);
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
            if (value_may_carry(storage, dst_type)) {
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
                    path[instr.dst.idx] = loans[loan].place.path;
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
          u32 referent = NO_ROOT;
          std::vector<u32> referent_path;
          for (u32 loan : flow[arg.as_register().idx]) {
            if (loan < loans.size() && loans[loan].place.root != NO_ROOT) {
              referent = loans[loan].place.root;
              referent_path = loans[loan].place.path;
              break;
            }
          }
          if (referent == NO_ROOT) {
            continue;
          }
          Place place{referent, referent_path};
          for (u32 step : entry.path) {
            place.path.push_back(step);
          }
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
                const std::vector<u32>& wide = loans[loan].place.path;
                const std::vector<u32>& narrow = proj.place.path;
                if (wide.size() < narrow.size()) {
                  bool prefix = true;
                  for (usize i = 0; i < wide.size(); ++i) {
                    if (!steps_match(wide[i], narrow[i])) {
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
          path[instr.dst.idx] = projected.front().place.path;
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
    loan_last_use.assign(storage.blocks().size(), {});
    live_in.assign(storage.blocks().size(), {});
    live_out.assign(storage.blocks().size(), {});
    for (std::vector<u32>& row : loan_last_use) {
      row.assign(loans.size(), 0);
    }
    // A loan is last read at the last instruction that reads any
    // register carrying it, in the block that instruction is in.
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
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
            if (loan < loan_last_use[bidx.idx].size() &&
                iidx.idx > loan_last_use[bidx.idx][loan]) {
              loan_last_use[bidx.idx][loan] = iidx.idx;
            }
          }
        }
      }
    }
    std::vector<std::vector<ir::BlockIdx>> preds(storage.blocks().size());
    std::vector<ir::BlockIdx> succs;
    for (ir::BlockIdx bidx : fn.blocks) {
      successors(bidx, succs);
      for (ir::BlockIdx succ : succs) {
        if (succ.idx < preds.size()) {
          preds[succ.idx].push_back(bidx);
        }
      }
    }
    auto add_loan = [](std::vector<u32>& set, u32 loan) {
      for (u32 prior : set) {
        if (prior == loan) {
          return;
        }
      }
      set.push_back(loan);
    };
    // Backward to a fixed point: a block is live out wherever any
    // successor is live in, and live in wherever it is read on a path
    // that reaches one of them.
    std::vector<u32> block_first(storage.blocks().size(), 0);
    std::vector<u32> block_last(storage.blocks().size(), 0);
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
      if (block.instrs.empty()) {
        continue;
      }
      block_first[bidx.idx] = block.instrs.head().idx;
      block_last[bidx.idx] = block.instrs.head().idx + block.instrs.size() - 1;
    }
    const usize cap = order.size() * 10 + 10;
    for (usize iter = 0; iter < cap; ++iter) {
      bool changed = false;
      for (ir::BlockIdx bidx : order) {
        std::vector<u32> out;
        succs.clear();
        successors(bidx, succs);
        for (ir::BlockIdx succ : succs) {
          for (u32 loan : live_in[succ.idx]) {
            add_loan(out, loan);
          }
        }
        if (out != live_out[bidx.idx]) {
          live_out[bidx.idx] = std::move(out);
          changed = true;
        }
        // A loan is live on entry when it is read in this block or live
        // on exit, and it was not born here: a loan born in the block
        // starts at its own birth, so it is never live before it. That
        // exclusion is what stops a loan made in a loop body from
        // wrapping around the back edge and being live on the next
        // entry, where it would outlive the iteration that made it.
        std::vector<u32> in;
        for (u32 loan = 0; loan < loans.size(); ++loan) {
          const u32 birth = loans[loan].birth;
          if (birth >= block_first[bidx.idx] && birth <= block_last[bidx.idx]) {
            continue;
          }
          bool live_here = loan_last_use[bidx.idx][loan] != 0;
          for (u32 prior : live_out[bidx.idx]) {
            if (prior == loan) {
              live_here = true;
              break;
            }
          }
          if (live_here) {
            add_loan(in, loan);
          }
        }
        if (in != live_in[bidx.idx]) {
          live_in[bidx.idx] = std::move(in);
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
    return loan < loan_last_use[bidx.idx].size() &&
           loan_last_use[bidx.idx][loan] >= pos;
  }

  void check_place_use(const std::vector<Place>& moved,
                       const Place& place,
                       diag::Span span,
                       std::string_view action) {
    for (const Place& gone : moved) {
      if (overlaps(gone, place)) {
        const u32 index = bag.emit<i18n::Key::BorrowUseAfterMove>(
            diag::Severity::Error, diag::Stage::Borrow, DiagCode::UseAfterMove,
            span, addr_name(place.root), action);
        (void)index;
        return;
      }
    }
  }

  static bool same_place(const Place& a, const Place& b) {
    return a.root == b.root && a.path == b.path;
  }

  static bool same_state(const std::vector<Place>& a,
                         const std::vector<Place>& b) {
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

  static void add_moved(std::vector<Place>& state, const Place& place) {
    for (const Place& prior : state) {
      if (same_place(prior, place)) {
        return;
      }
    }
    state.push_back(place);
  }

  static void kill_moved(std::vector<Place>& state, const Place& place) {
    for (usize i = 0; i < state.size();) {
      if (overlaps(state[i], place)) {
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
    std::vector<bool> seen(storage.blocks().size(), false);
    std::vector<ir::BlockIdx> stack;
    std::vector<ir::BlockIdx> post;
    stack.push_back(fn.blocks.head());
    seen[fn.blocks.head().idx] = true;
    std::vector<ir::BlockIdx> succs;
    while (!stack.empty()) {
      const ir::BlockIdx top = stack.back();
      succs.clear();
      successors(top, succs);
      bool descended = false;
      for (ir::BlockIdx succ : succs) {
        if (succ.idx < seen.size() && !seen[succ.idx]) {
          seen[succ.idx] = true;
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
      if (bidx.idx < seen.size() && !seen[bidx.idx]) {
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
  void compute_moved(const ir::Function& fn,
                     const std::vector<ir::BlockIdx>& order) {
    moved_in.assign(storage.blocks().size(), {});
    moved_out.assign(storage.blocks().size(), {});
    std::vector<std::vector<ir::BlockIdx>> preds(storage.blocks().size());
    std::vector<ir::BlockIdx> succs;
    for (ir::BlockIdx bidx : fn.blocks) {
      succs.clear();
      successors(bidx, succs);
      for (ir::BlockIdx succ : succs) {
        if (succ.idx < preds.size()) {
          preds[succ.idx].push_back(bidx);
        }
      }
    }
    std::vector<Place> in;
    std::vector<Place> out;
    const usize cap = order.size() * 10 + 10;
    for (usize iter = 0; iter < cap; ++iter) {
      bool changed = false;
      for (ir::BlockIdx bidx : order) {
        in.clear();
        for (ir::BlockIdx pred : preds[bidx.idx]) {
          for (const Place& place : moved_out[pred.idx]) {
            add_moved(in, place);
          }
        }
        transfer(bidx, in, out);
        // IN is refreshed every visit for the check pass to read;
        // successors are fed from OUT, which alone drives convergence.
        moved_in[bidx.idx] = in;
        if (!same_state(out, moved_out[bidx.idx])) {
          moved_out[bidx.idx] = out;
          changed = true;
        }
      }
      if (!changed) {
        return;
      }
    }
    DCHECK(false);
  }

  void check_function(const ir::Function& fn) {
    forward(fn);
    std::vector<ir::BlockIdx> order;
    reverse_post_order(fn, order);
    compute_moved(fn, order);
    compute_liveness(fn, order);
    std::vector<Place> state;
    for (ir::BlockIdx bidx : order) {
      state = moved_in[bidx.idx];
      const ir::Block& block = storage.blocks()[bidx];
      for (ir::InstructionIdx iidx : block.instrs) {
        check_instr(fn, bidx, iidx, state);
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
            if (!live_at(id, bidx, pos) || !overlaps(loan.place, place)) {
              continue;
            }
            const u32 index = bag.emit<i18n::Key::BorrowConflict>(
                diag::Severity::Error, diag::Stage::Borrow, DiagCode::Conflict,
                span, addr_name(place.root));
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
        check_place_use(moved, place, span, "move");
        for (u32 id = 0; id < static_cast<u32>(loans.size()); ++id) {
          const Loan& loan = loans[id];
          if (loan.param != NO_ROOT) {
            continue;
          }
          if (!live_at(id, bidx, pos) || !overlaps(loan.place, place)) {
            continue;
          }
          const u32 index = bag.emit<i18n::Key::BorrowMoveInvalidatesBorrow>(
              diag::Severity::Error, diag::Stage::Borrow,
              DiagCode::UseAfterMove, span, addr_name(place.root));
          (void)index;
          break;
        }
        add_moved(moved, place);
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
          if (!live_at(id, bidx, pos) || !overlaps(loan.place, place)) {
            continue;
          }
          if (exclusive || loan.exclusive) {
            const u32 index = bag.emit<i18n::Key::BorrowConflict>(
                diag::Severity::Error, diag::Stage::Borrow, DiagCode::Conflict,
                span, addr_name(place.root));
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
            if (!live_at(id, bidx, pos) || !overlaps(loan.place, place)) {
              continue;
            }
            const u32 index = bag.emit<i18n::Key::BorrowAssignWhileBorrowed>(
                diag::Severity::Error, diag::Stage::Borrow,
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
          if (!is_param_root(root, fn)) {
            const u32 index = bag.emit<i18n::Key::BorrowReturnsLocalReference>(
                diag::Severity::Error, diag::Stage::Borrow, DiagCode::Escape,
                span, addr_name(root));
            (void)index;
            break;
          }
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
    std::vector<u32> param_of_alloca(storage.registers().size(), NO_ROOT);
    for (const auto& [alloca, index] : param_homes) {
      if (alloca < param_of_alloca.size()) {
        param_of_alloca[alloca] = index;
      }
    }
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
          if (entry.place.root >= param_of_alloca.size() ||
              param_of_alloca[entry.place.root] == NO_ROOT) {
            continue;
          }
          param = param_of_alloca[entry.place.root];
          relpath = entry.place.path;
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

  void reset_function() {
    // Per-function scratch shares global register indexes.
    home.assign(storage.registers().size(), NO_ROOT);
    path.assign(storage.registers().size(), {});
    flow.assign(storage.registers().size(), {});
    loans.clear();
    moved_in.clear();
    moved_out.clear();
    param_homes.clear();
  }

  void run() {
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
        reset_function();
        const ir::Function& fn = storage.functions()[fidx];
        {
          PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, fn_name(fn),
                                                   "borrow-fn");
          forward(fn);
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
      reset_function();
      const ir::Function& fn = storage.functions()[fidx];
      {
        PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, fn_name(fn),
                                                 "borrow-fn");
        check_function(fn);
      }
    }
  }
};

}  // namespace

base::Result<void, diag::Reported> check_borrows(
    const lowering::LoweredPackage& lowered,
    diag::DiagBag& bag,
    str::StringInterner& strings,
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
