// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "borrow/borrow.h"

#include <string_view>
#include <utility>
#include <vector>

#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lower/lower.h"

namespace borrow {

namespace {

// Diagnostic codes 4400-4409 are reserved for borrow checking.
constexpr u32 kBorrowUseAfterMove = 4400;
constexpr u32 kBorrowConflict = 4401;
constexpr u32 kBorrowEscape = 4402;
constexpr u32 kBorrowAssignBorrowed = 4403;

constexpr u32 kNoRoot = 0xFFFFFFFFu;

// A place: a root register (alloca or block parameter) plus a field
// path. Moves, borrows, and revives name overlapping places: one
// path prefixes the other (or they are equal).
struct Place {
  u32 root = kNoRoot;
  std::vector<u32> path;
};

bool overlaps(const Place& a, const Place& b) {
  if (a.root != b.root) {
    return false;
  }
  const usize common =
      a.path.size() < b.path.size() ? a.path.size() : b.path.size();
  for (usize i = 0; i < common; ++i) {
    if (a.path[i] != b.path[i]) {
      return false;
    }
  }
  return true;
}

struct Loan {
  u32 reg = kNoRoot;
  Place place;
  bool exclusive = false;
  u32 birth = 0;
  u32 expiry = 0;
  // Summary token for a parameter alloca: carries the parameter
  // index so return-reachability becomes a summary. Never conflicts;
  // reification propagates the caller's own loans instead.
  u32 param = kNoRoot;
};

struct Checker {
  Checker(const lower::LoweredPackage& lowered,
          const ir::Storage& storage,
          diag::DiagBag& bag)
      : lowered(lowered), storage(storage), bag(bag) {}

  const lower::LoweredPackage& lowered;
  const ir::Storage& storage;
  diag::DiagBag& bag;

  // Per-function scratch, indexed by global register.
  std::vector<u32> home;
  std::vector<std::vector<u32>> path;
  std::vector<std::vector<u32>> flow;
  std::vector<u32> last_use;
  std::vector<Loan> loans;
  // Move states per block, indexed by global block. Joins union
  // predecessor states (a use after a maybe-move is an error);
  // sibling branches stay independent through CFG predecessors.
  // Checks seed from moved_in (before the block's own moves).
  std::vector<std::vector<Place>> moved_in;
  std::vector<std::vector<Place>> moved_out;
  // Function summaries (return-regions): parameter indexes whose
  // loans may reach a return, indexed by function. Call sites
  // reify them by propagating only those arguments' loans.
  std::vector<std::vector<u32>> summaries;

  const ir::Instruction& instr_at(ir::InstructionIdx idx) const {
    return storage.instrs()[idx];
  }

  ir::TypeTag tag_of(ir::TypeIdx idx) const { return storage.types()[idx].tag; }

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
    // Parameter home allocas (entry stores of block parameters).
    const ir::Block& entry = storage.blocks()[fn.blocks.head()];
    for (ir::InstructionIdx iidx : entry.instrs) {
      const ir::Instruction& instr = instr_at(iidx);
      if (instr.op != ir::Opcode::Store || instr.operands.size() != 2) {
        continue;
      }
      const ir::Operand& value = storage.operands()[instr.operands.head()];
      const ir::Operand& target = storage.operands()[instr.operands.head() + 1];
      if (!value.is<ir::RegisterIdx>() || !target.is<ir::RegisterIdx>() ||
          target.as_register().idx != reg) {
        continue;
      }
      for (ir::BlockParamIdx pidx : entry.block_params) {
        if (storage.block_params()[pidx].reg.idx == value.as_register().idx) {
          return true;
        }
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
    if (reg >= home.size() || home[reg] == kNoRoot) {
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
    std::vector<std::pair<u32, u32>> params;
    param_allocas(fn, params);
    for (const auto& [alloca, index] : params) {
      if (alloca >= flow.size()) {
        continue;
      }
      Place place;
      place.root = alloca;
      loans.push_back({alloca, place, false, 0, 0, index});
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
        u32 addr = kNoRoot;
        if (operand_reg(0, addr) && instr.dst.is_valid()) {
          flow[instr.dst.idx] = flow[addr];
        }
        break;
      }
      case ir::Opcode::GetElementPtr: {
        u32 base = kNoRoot;
        if (!operand_reg(0, base) || !instr.dst.is_valid()) {
          break;
        }
        if (home[base] == kNoRoot) {
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
      case ir::Opcode::Move: {
        u32 src = kNoRoot;
        if (operand_reg(0, src) && instr.dst.is_valid()) {
          flow[instr.dst.idx] = flow[src];
        }
        break;
      }
      case ir::Opcode::Borrow: {
        u32 place_reg = kNoRoot;
        if (!operand_reg(0, place_reg) || !instr.dst.is_valid()) {
          break;
        }
        Place place;
        if (place_of(storage.operands()[instr.operands.head()], place)) {
          flow[instr.dst.idx].push_back(static_cast<u32>(loans.size()));
          const bool exclusive = tag_of(storage.registers()[instr.dst].type) ==
                                 ir::TypeTag::MutRef;
          loans.push_back({instr.dst.idx, place, exclusive, pos, pos});
        }
        break;
      }
      case ir::Opcode::Store: {
        u32 value = kNoRoot;
        u32 addr = kNoRoot;
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
        // External calls have no summary; their results carry no
        // caller loans (no such external exists in the surface
        // language).
        if (!callee.is<ir::FunctionIdx>() || !instr.dst.is_valid()) {
          break;
        }
        const ir::FunctionIdx target = callee.as_function();
        if (target.idx >= summaries.size()) {
          break;
        }
        for (u32 param : summaries[target.idx]) {
          if (param + 1 >= instr.operands.size()) {
            break;
          }
          const ir::Operand& arg =
              storage.operands()[instr.operands.head() + param + 1];
          if (!arg.is<ir::RegisterIdx>() ||
              arg.as_register().idx >= flow.size()) {
            continue;
          }
          for (u32 loan : flow[arg.as_register().idx]) {
            bool known = false;
            for (u32 prior : flow[instr.dst.idx]) {
              if (prior == loan) {
                known = true;
                break;
              }
            }
            if (!known) {
              flow[instr.dst.idx].push_back(loan);
            }
          }
        }
        break;
      }
      default: break;
    }
    // Last-use positions for every register operand.
    for (u32 offset = 0; offset < instr.operands.size(); ++offset) {
      const ir::Operand& operand =
          storage.operands()[instr.operands.head() + offset];
      if (operand.is<ir::RegisterIdx>() &&
          operand.as_register().idx < last_use.size()) {
        last_use[operand.as_register().idx] = pos;
      }
    }
  }

  void compute_expiry() {
    // A loan stays live through every use of every value derived
    // from it (moves, loads, and aggregate holders propagate the
    // identity forward), so sequential borrows of one place expire
    // while interleaved ones stay live.
    for (Loan& loan : loans) {
      loan.expiry = loan.birth;
    }
    for (u32 reg = 0; reg < static_cast<u32>(flow.size()); ++reg) {
      for (u32 loan : flow[reg]) {
        if (loan < loans.size() && last_use[reg] > loans[loan].expiry) {
          loans[loan].expiry = last_use[reg];
        }
      }
    }
  }

  bool live_at(const Loan& loan, u32 pos) const {
    return loan.birth <= pos && pos <= loan.expiry;
  }

  void check_place_use(const std::vector<Place>& moved,
                       const Place& place,
                       diag::Span span,
                       std::string_view action) {
    for (const Place& gone : moved) {
      if (overlaps(gone, place)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kBorrowUseAfterMove, span,
                     "use of moved '{}' in {}", addr_name(place.root), action);
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
        // operands = [value, default, (case_imm, case_block)...].
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
        // IN refreshes every visit (a changed IN with an unchanged OUT
        // still feeds successors); only OUT changes drive convergence.
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
    compute_expiry();
    std::vector<ir::BlockIdx> order;
    reverse_post_order(fn, order);
    compute_moved(fn, order);
    std::vector<Place> state;
    for (ir::BlockIdx bidx : order) {
      state = moved_in[bidx.idx];
      const ir::Block& block = storage.blocks()[bidx];
      for (ir::InstructionIdx iidx : block.instrs) {
        check_instr(fn, iidx, state);
      }
    }
  }

  void check_instr(const ir::Function& fn,
                   ir::InstructionIdx iidx,
                   std::vector<Place>& moved) {
    const ir::Instruction& instr = instr_at(iidx);
    const u32 pos = iidx.idx;
    const diag::Span span = iidx.idx < lowered.instr_spans.size()
                                ? lowered.instr_spans[iidx.idx]
                                : diag::Span{};
    auto operand_place = [&](u32 offset, Place& place) {
      if (offset >= instr.operands.size()) {
        return false;
      }
      return place_of(storage.operands()[instr.operands.head() + offset],
                      place);
    };
    switch (instr.op) {
      case ir::Opcode::Move: {
        Place place;
        if (!operand_place(0, place)) {
          break;
        }
        check_place_use(moved, place, span, "move");
        for (const Loan& loan : loans) {
          if (loan.param != kNoRoot) {
            continue;
          }
          if (!live_at(loan, pos) || !overlaps(loan.place, place)) {
            continue;
          }
          const u32 index =
              bag.emit(diag::Severity::Error, kBorrowUseAfterMove, span,
                       "move of '{}' invalidates an outstanding borrow",
                       addr_name(place.root));
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
        for (const Loan& loan : loans) {
          if (loan.reg == instr.dst.idx || loan.param != kNoRoot) {
            continue;
          }
          if (!live_at(loan, pos) || !overlaps(loan.place, place)) {
            continue;
          }
          if (exclusive || loan.exclusive) {
            const u32 index =
                bag.emit(diag::Severity::Error, kBorrowConflict, span,
                         "conflicting borrows of '{}'", addr_name(place.root));
            (void)index;
            break;
          }
        }
        break;
      }
      case ir::Opcode::Store: {
        Place place;
        if (operand_place(1, place)) {
          kill_moved(moved, place);
          // Assignment invalidates outstanding borrows of the place.
          for (const Loan& loan : loans) {
            if (loan.param != kNoRoot) {
              continue;
            }
            if (!live_at(loan, pos) || !overlaps(loan.place, place)) {
              continue;
            }
            const u32 index = bag.emit(
                diag::Severity::Error, kBorrowAssignBorrowed, span,
                "cannot assign to '{}' while borrowed", addr_name(place.root));
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
          if (loan >= loans.size() || loans[loan].param != kNoRoot) {
            continue;
          }
          const u32 root = loans[loan].place.root;
          if (!is_param_root(root, fn)) {
            const u32 index =
                bag.emit(diag::Severity::Error, kBorrowEscape, span,
                         "returns reference to local '{}'", addr_name(root));
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

  // Recomputes one summary from current flow: parameter indexes
  // whose loans reach a return. Summaries only grow across sweeps.
  bool update_summary(ir::FunctionIdx fidx) {
    const ir::Function& fn = storage.functions()[fidx];
    std::vector<u32> next;
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
        if (loan >= loans.size() || loans[loan].param == kNoRoot) {
          continue;
        }
        bool known = false;
        for (u32 prior : next) {
          if (prior == loans[loan].param) {
            known = true;
            break;
          }
        }
        if (!known) {
          next.push_back(loans[loan].param);
        }
      }
    }
    if (next.size() == summaries[fidx.idx].size()) {
      bool same = true;
      for (u32 index : next) {
        bool found = false;
        for (u32 prior : summaries[fidx.idx]) {
          if (prior == index) {
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
    summaries[fidx.idx] = next;
    return true;
  }

  void reset_function() {
    // Per-function scratch shares global register indexes.
    home.assign(storage.registers().size(), kNoRoot);
    path.assign(storage.registers().size(), {});
    flow.assign(storage.registers().size(), {});
    last_use.assign(storage.registers().size(), 0);
    loans.clear();
    moved_in.clear();
    moved_out.clear();
  }

  void run() {
    summaries.assign(storage.functions().size(), {});
    // Phase A: bounded summary fixed-point over the call graph.
    // Sweeping all functions propagates one call edge per sweep;
    // chains longer than the function count cannot exist, so the
    // cap only fires on compiler bugs.
    const usize cap = storage.functions().size() + 1;
    bool stable = false;
    for (usize sweep = 0; sweep < cap && !stable; ++sweep) {
      stable = true;
      for (ir::FunctionIdx fidx(0); fidx.idx < storage.functions().size();
           ++fidx) {
        reset_function();
        forward(storage.functions()[fidx]);
        if (update_summary(fidx)) {
          stable = false;
        }
      }
    }
    if (!stable) {
      DCHECK(false);
    }
    // Phase B: checking with final summaries.
    for (ir::FunctionIdx fidx(0); fidx.idx < storage.functions().size();
         ++fidx) {
      reset_function();
      check_function(storage.functions()[fidx]);
    }
  }
};

}  // namespace

void check_borrows(const lower::LoweredPackage& lowered, diag::DiagBag& bag) {
  Checker checker{lowered, lowered.storage, bag};
  checker.run();
}

}  // namespace borrow
