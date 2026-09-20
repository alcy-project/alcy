// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "borrow/borrow.h"

#include <string_view>
#include <utility>
#include <vector>

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
  std::vector<Place> moved;

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
    return false;
  }

  static bool is_ref_tag(ir::TypeTag tag) {
    return tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef ||
           tag == ir::TypeTag::Ptr;
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

  void forward(const ir::Function& fn) {
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
        for (u32 loan : flow[value]) {
          bool known = false;
          for (u32 prior : flow[addr]) {
            if (prior == loan) {
              known = true;
              break;
            }
          }
          if (!known) {
            flow[addr].push_back(loan);
          }
        }
        break;
      }
      case ir::Opcode::Call: {
        if (instr.operands.empty()) {
          break;
        }
        const ir::Operand& callee = storage.operands()[instr.operands.head()];
        if (!callee.is<ir::FunctionIdx>() || !instr.dst.is_valid()) {
          break;
        }
        const ir::FunctionMeta& meta =
            storage.functions()[callee.as_function()].meta;
        for (u32 i = 1; i < instr.operands.size(); ++i) {
          if (i - 1 >= meta.param_types.size()) {
            break;
          }
          if (!is_ref_tag(tag_of(meta.param_types[i - 1]))) {
            continue;
          }
          const ir::Operand& arg =
              storage.operands()[instr.operands.head() + i];
          if (!arg.is<ir::RegisterIdx>()) {
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

  void check_place_use(const Place& place,
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

  void check_function(const ir::Function& fn) {
    forward(fn);
    compute_expiry();
    for (ir::BlockIdx bidx : fn.blocks) {
      const ir::Block& block = storage.blocks()[bidx];
      for (ir::InstructionIdx iidx : block.instrs) {
        check_instr(fn, iidx);
      }
    }
  }

  void check_instr(const ir::Function& fn, ir::InstructionIdx iidx) {
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
        check_place_use(place, span, "move");
        for (const Loan& loan : loans) {
          if (loan.expiry > pos && overlaps(loan.place, place)) {
            const u32 index =
                bag.emit(diag::Severity::Error, kBorrowUseAfterMove, span,
                         "move of '{}' invalidates an outstanding borrow",
                         addr_name(place.root));
            (void)index;
            break;
          }
        }
        moved.push_back(place);
        break;
      }
      case ir::Opcode::Borrow: {
        Place place;
        if (!operand_place(0, place)) {
          break;
        }
        check_place_use(place, span, "borrow");
        const bool exclusive =
            instr.dst.is_valid() &&
            tag_of(storage.registers()[instr.dst].type) == ir::TypeTag::MutRef;
        for (const Loan& loan : loans) {
          if (loan.reg == instr.dst.idx) {
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
          for (usize i = 0; i < moved.size();) {
            if (overlaps(moved[i], place)) {
              moved[i] = moved.back();
              moved.pop_back();
            } else {
              ++i;
            }
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
          if (loan >= loans.size()) {
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

  void run() {
    for (ir::FunctionIdx fidx(0); fidx.idx < storage.functions().size();
         ++fidx) {
      // Per-function scratch shares global register indexes.
      home.assign(storage.registers().size(), kNoRoot);
      path.assign(storage.registers().size(), {});
      flow.assign(storage.registers().size(), {});
      last_use.assign(storage.registers().size(), 0);
      loans.clear();
      moved.clear();
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
