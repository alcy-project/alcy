// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/select.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "codegen/backend.h"
#include "codegen/diag_code.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "codegen/x86_64/rodata.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace codegen::x86_64 {

namespace {

// The bytes one scalar value occupies, which is what the integer
// opcodes' width field asks for. Arithmetic runs at 32 or 64 bits and
// stores at the value's own width, so the narrow widths name moves and
// nothing else.
u8 width_of(const ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I1:
    case ir::TypeTag::I8:
    case ir::TypeTag::U8: return 1;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 2;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32:
    case ir::TypeTag::F32: return 4;
    default: return 8;
  }
}

// The width arithmetic runs at, which is the value's own width rounded
// up to the smallest the integer opcodes carry.
u8 compute_width_of(const ir::TypeTag tag) {
  const u8 width = width_of(tag);
  return width < 4 ? 4 : width;
}

constexpr bool is_signed(const ir::TypeTag tag) {
  return tag == ir::TypeTag::I1 || tag == ir::TypeTag::I8 ||
         tag == ir::TypeTag::I16 || tag == ir::TypeTag::I32 ||
         tag == ir::TypeTag::I64;
}

constexpr bool is_integer(const ir::TypeTag tag) {
  return is_signed(tag) || tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
         tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64;
}

constexpr bool is_float(const ir::TypeTag tag) {
  return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
}

// Whether this type rides in one or two registers, which is what the
// calling convention asks of it. A reference to a slice is the fat
// pointer the callers handle beside these.
bool is_two_word(const ir::TypeTag tag) {
  return tag == ir::TypeTag::Str || tag == ir::TypeTag::Func ||
         tag == ir::TypeTag::Slice;
}

constexpr bool is_aggregate(const ir::TypeTag tag) {
  return tag == ir::TypeTag::Struct || tag == ir::TypeTag::Array ||
         tag == ir::TypeTag::Enum || tag == ir::TypeTag::Tuple;
}

constexpr Cond compare_of(const ir::Opcode op, const bool plus_signed) {
  switch (op) {
    case ir::Opcode::Eq: return Cond::E;
    case ir::Opcode::Ne: return Cond::Ne;
    case ir::Opcode::Lt: return plus_signed ? Cond::L : Cond::B;
    case ir::Opcode::Le: return plus_signed ? Cond::Le : Cond::Be;
    case ir::Opcode::Gt: return plus_signed ? Cond::G : Cond::A;
    case ir::Opcode::Ge: return plus_signed ? Cond::Ge : Cond::Ae;
    default: return Cond::E;
  }
}

// The two-operand opcode one IR arithmetic shape lowers to. Multiply and
// divide have their own forms and their own cases.
constexpr MOp alu_of(const ir::Opcode op) {
  switch (op) {
    case ir::Opcode::IntAdd:
    case ir::Opcode::IntAddWrap: return MOp::Add;
    case ir::Opcode::IntSub:
    case ir::Opcode::IntSubWrap: return MOp::Sub;
    case ir::Opcode::And: return MOp::And;
    case ir::Opcode::Or: return MOp::Or;
    case ir::Opcode::Xor: return MOp::Xor;
    default: return MOp::Add;
  }
}

// The shift opcode's slot, shared by every width.
enum class ShiftOp : u8 { Shl = 4, Shr = 5, Sar = 7 };

constexpr ShiftOp shift_of(const ir::Opcode op) {
  switch (op) {
    case ir::Opcode::ShiftLeft: return ShiftOp::Shl;
    case ir::Opcode::LogicalShiftRight: return ShiftOp::Shr;
    default: return ShiftOp::Sar;
  }
}

constexpr MOp shift_mop_of(const ir::Opcode op) {
  switch (shift_of(op)) {
    case ShiftOp::Shl: return MOp::Shl;
    case ShiftOp::Shr: return MOp::Shr;
    default: return MOp::Sar;
  }
}

}  // namespace

std::string internal_symbol(const ir::FunctionIdx index) {
  return "alcy.fn." + std::to_string(index.idx);
}

namespace {

class Selector {
 public:
  Selector(const ir::Storage& storage,
           const ir::SymbolTable& strings,
           std::span<const diag::Span> spans,
           const ir::Function& function,
           diag::DiagBag& bag,
           const ir::PointerWidth width,
           RoData& rodata)
      : storage_(storage),
        strings_(strings),
        spans_(spans),
        function_(function),
        bag_(bag),
        width_(width),
        rodata_(rodata) {}

  base::Result<Body, codegen::EmitError> run();

 private:
  // A construct with no lowering, at the place it was written.
  base::Result<Body, codegen::EmitError> unsupported(const diag::Span span,
                                                     std::string_view what);

  // The vreg one IR register selects to, making one on first use. The
  // register indexes are the module's, so the map is what keeps a body's
  // vregs dense.
  u32 vreg_of(ir::RegisterIdx reg, ir::TypeIdx type);

  diag::Span span_of(const ir::InstructionIdx index) const {
    return index.idx < spans_.size() ? spans_[index.idx] : diag::Span{};
  }

  // One instruction's lowering. Returns false when it reported a
  // construct it has no lowering for.
  [[nodiscard]] bool emit(const ir::Instruction& instr, const diag::Span span);

  // The operand as the machine reads it: a register as its vreg, an
  // immediate as itself, a block as its label.
  [[nodiscard]] std::optional<Operand> operand_of(const ir::Operand& op);

  // An integer or pointer operand, which is what a scalar instruction
  // reads. A shape it cannot take is reported and answered with
  // nothing.
  [[nodiscard]] std::optional<Operand> scalar_of(const ir::Operand& op,
                                                 const diag::Span span);

  // The type a pointer register addresses, from the instruction that
  // made it. `GetElementPtr` has no type of its own to read, so the
  // projections that follow it read this.
  [[nodiscard]] std::optional<ir::TypeIdx> pointee_hint(
      ir::RegisterIdx reg) const;

  void note_pointee(ir::RegisterIdx reg, ir::TypeIdx type);

  // The symbol reference one string's bytes answer to, in this body's
  // own table.
  [[nodiscard]] SymbolRef string_ref(const std::string& symbol);

  const ir::Storage& storage_;
  const ir::SymbolTable& strings_;
  std::span<const diag::Span> spans_;
  const ir::Function& function_;
  diag::DiagBag& bag_;
  ir::PointerWidth width_;
  RoData& rodata_;

  Body body_;
  std::unordered_map<u32, u32> vreg_of_register_;
  std::unordered_map<u32, ir::TypeIdx> pointee_of_register_;
  bool failed_ = false;
  // Whether this block's first instruction has been placed, which is
  // what decides where its label sits.
  bool placed_ = false;
  u32 label_ = 0;
};

u32 Selector::vreg_of(const ir::RegisterIdx reg, const ir::TypeIdx type) {
  const auto found = vreg_of_register_.find(reg.idx);
  if (found != vreg_of_register_.end()) {
    return found->second;
  }
  const u32 index = static_cast<u32>(body_.vreg_types.size());
  body_.vreg_types.push_back(type.idx);
  vreg_of_register_.emplace(reg.idx, index);
  return index;
}

void Selector::note_pointee(const ir::RegisterIdx reg, const ir::TypeIdx type) {
  pointee_of_register_.emplace(reg.idx, type);
}

std::optional<ir::TypeIdx> Selector::pointee_hint(
    const ir::RegisterIdx reg) const {
  const auto found = pointee_of_register_.find(reg.idx);
  if (found != pointee_of_register_.end()) {
    return found->second;
  }
  // A register the body only received carries its own answer: a
  // reference names its pointee.
  const ir::TypeIdx type = storage_.registers()[reg].type;
  const ir::TypeTag tag = storage_.types()[type.idx].tag;
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    return storage_.ref_types()[storage_.types()[type.idx].as_ref()].pointee;
  }
  return std::nullopt;
}

SymbolRef Selector::string_ref(const std::string& symbol) {
  for (u32 at = 0; at < body_.symbols.size(); ++at) {
    if (body_.symbols[at] == symbol) {
      return SymbolRef{at};
    }
  }
  body_.symbols.push_back(symbol);
  return SymbolRef{static_cast<u32>(body_.symbols.size() - 1)};
}

base::Result<Body, codegen::EmitError> Selector::unsupported(
    const diag::Span span,
    const std::string_view what) {
  if (!failed_) {
    (void)bag_.emit<i18n::Key::CodegenX86Unsupported>(
        codegen::DiagCode::Unsupported, span, what);
    failed_ = true;
  }
  return base::make_err(codegen::EmitError::Unsupported);
}

std::optional<Operand> Selector::operand_of(const ir::Operand& op) {
  if (op.is<ir::RegisterIdx>()) {
    return Operand{VReg{vreg_of(op.as_register(), op.type)}};
  }
  if (op.is<ir::ImmutableIdx>()) {
    const ir::Immutable& imm = storage_.immutables()[op.as_immutable()];
    const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
    if (is_integer(tag)) {
      return Operand{Imm{static_cast<i64>(imm.as_u64_integer(tag))}};
    }
    if (tag == ir::TypeTag::Str) {
      // A string literal is the two words its reference carries, and
      // the data section holds the bytes once.
      return Operand{
          string_ref(rodata_.intern(strings_.get(imm.data.str_id_value)))};
    }
  }
  if (op.is<ir::BlockIdx>()) {
    const ir::BlockIdx block = op.as_block();
    return Operand{Label{body_.block_labels[block.idx]}};
  }
  return std::nullopt;
}

std::optional<Operand> Selector::scalar_of(const ir::Operand& op,
                                           const diag::Span span) {
  const ir::TypeTag tag = storage_.types()[op.type].tag;
  if (is_aggregate(tag) || is_two_word(tag)) {
    (void)unsupported(span,
                      "a value wider than a register in a scalar instruction");
    return std::nullopt;
  }
  if (const std::optional<Operand> spelled = operand_of(op);
      spelled.has_value()) {
    return spelled;
  }
  // A shape the selector has no spelling for: a float constant today,
  // whose bits are not an integer immediate.
  (void)unsupported(span, "an operand the backend cannot spell");
  return std::nullopt;
}

base::Result<Body, codegen::EmitError> Selector::run() {
  // Every block the body carries gets a label, whether or not a branch
  // names it: a block's first instruction is where its label sits, and
  // the map from block to label is what the branches read.
  for (const ir::BlockIdx block_idx : function_.blocks) {
    body_.block_labels.emplace(block_idx.idx, body_.label_count++);
  }
  for (const ir::BlockIdx block_idx : function_.blocks) {
    label_ = body_.block_labels[block_idx.idx];
    placed_ = false;
    const ir::Block& block = storage_.blocks()[block_idx];
    for (const ir::InstructionIdx instr_idx : block.instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      if (!emit(instr, span_of(instr_idx))) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
    }
  }
  if (failed_) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  return base::make_ok(std::move(body_));
}

bool Selector::emit(const ir::Instruction& instr, const diag::Span span) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const auto at = [&](const u32 index) {
    return storage_.operands()[ops.head() + index];
  };
  const usize count = ops.size();

  const auto push = [&](MInst inst) {
    if (!placed_) {
      inst.label = label_;
      placed_ = true;
    }
    body_.insts.push_back(inst);
  };

  const bool has_dst = instr.dst.is_valid();
  const auto dst_type_of = [&]() {
    return storage_.registers()[instr.dst].type;
  };
  const auto dst_tag = [&]() {
    return storage_.types()[dst_type_of().idx].tag;
  };
  const auto dst_operand = [&]() {
    return Operand{VReg{vreg_of(instr.dst, dst_type_of())}};
  };
  // A fresh label, for the shapes one instruction grows into.
  const auto fresh_label = [&]() { return body_.label_count++; };

  // The register an index rides on, when it is one. A constant index is
  // the caller's to fold into a displacement.
  const auto regref_of =
      [](const std::optional<Operand>& operand) -> std::optional<RegRef> {
    if (!operand.has_value()) {
      return std::nullopt;
    }
    if (const VReg* vreg = std::get_if<VReg>(&*operand); vreg != nullptr) {
      return RegRef{*vreg};
    }
    return std::nullopt;
  };

  switch (instr.op) {
    case O::Noop:
    case O::Drop:
    case O::ParametricDrop: {
      // Ownership markers only.
      return true;
    }
    case O::Move:
    case O::Borrow: {
      // A register alias. Slots name the same bytes only through a copy,
      // which is what a move of a wide value costs anyway.
      DCHECK(count == 1);
      if (!has_dst) {
        return true;
      }
      const std::optional<Operand> src = operand_of(at(0));
      if (!src.has_value()) {
        (void)unsupported(span, "a move of a value the backend cannot spell");
        return false;
      }
      push(make_inst(MOp::Mov, 8, dst_operand(), *src));
      return true;
    }
    case O::Ret: {
      // An explicit `()` comes back as an operand of a type that holds
      // no bytes, and the function still returns void.
      if (!ops.empty()) {
        const ir::Operand& value = at(0);
        const ir::TypeTag tag = storage_.types()[value.type].tag;
        if (tag != ir::TypeTag::Void && tag != ir::TypeTag::Never) {
          const ir::TypeLayout layout = storage_.layout_of(value.type, width_);
          if (layout.size > 16) {
            (void)unsupported(span, "a return value wider than two words");
            return false;
          }
          if (value.is<ir::RegisterIdx>()) {
            const u32 vreg = vreg_of(value.as_register(), value.type);
            // One word answers in `rax`; two ride in the pair.
            push(
                make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{VReg{vreg}}));
            if (layout.size > 8) {
              push(make_inst(MOp::Mov, 8, Operand{Reg::Rdx},
                             Operand{Mem{.base = VReg{vreg},
                                         .index = std::nullopt,
                                         .scale = 1,
                                         .disp = 8}}));
            }
          } else if (const std::optional<Operand> constant = operand_of(value);
                     constant.has_value() &&
                     std::holds_alternative<Imm>(*constant)) {
            push(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, *constant));
          } else {
            (void)unsupported(span,
                              "a constant return the backend cannot "
                              "spell");
            return false;
          }
        }
      }
      push(make_inst(MOp::Ret, 8));
      return true;
    }
    case O::Unreachable: {
      push(make_inst(MOp::Ud2, 8));
      return true;
    }
    case O::Br: {
      // operands[0] = target block, operands[1..] = its parameters.
      DCHECK(!ops.empty());
      const ir::BlockIdx target = at(0).as_block();
      const ir::Block& target_block = storage_.blocks()[target];
      DCHECK(count == 1 + target_block.block_params.size());
      for (u32 param = 0; param + 1 < count; ++param) {
        const ir::BlockParam& block_param =
            storage_.block_params()[target_block.block_params[param]];
        const std::optional<Operand> value = scalar_of(at(param + 1), span);
        if (!value.has_value()) {
          return false;
        }
        push(make_inst(
            MOp::Mov, 8,
            Operand{VReg{vreg_of(block_param.reg, block_param.type)}}, *value));
      }
      push(make_inst(MOp::Jmp, 8,
                     Operand{Label{body_.block_labels[target.idx]}}));
      return true;
    }
    case O::CondBr: {
      // operands = [cond, true_block, false_block].
      DCHECK(count == 3);
      const ir::BlockIdx taken = at(1).as_block();
      const ir::BlockIdx other = at(2).as_block();
      if (at(0).is<ir::ImmutableIdx>()) {
        // A constant condition is the branch it names.
        const ir::Immutable& imm = storage_.immutables()[at(0).as_immutable()];
        const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
        const bool truth = is_integer(tag) && imm.as_u64_integer(tag) != 0;
        push(make_inst(
            MOp::Jmp, 8,
            Operand{Label{body_.block_labels[(truth ? taken : other).idx]}}));
        return true;
      }
      const std::optional<Operand> cond = scalar_of(at(0), span);
      if (!cond.has_value()) {
        return false;
      }
      push(make_inst(MOp::Cmp,
                     compute_width_of(storage_.types()[at(0).type].tag), *cond,
                     Operand{Imm{0}}));
      push(make_jcc(Cond::Ne, Operand{Label{body_.block_labels[taken.idx]}}));
      push(make_inst(MOp::Jmp, 8,
                     Operand{Label{body_.block_labels[other.idx]}}));
      return true;
    }
    case O::Switch: {
      // operands = [value, default, (case_imm, case_block)...].
      DCHECK(count >= 2);
      const std::optional<Operand> value = scalar_of(at(0), span);
      if (!value.has_value()) {
        return false;
      }
      const u8 width = compute_width_of(storage_.types()[at(0).type].tag);
      for (u32 idx = 2; idx + 1 < count; idx += 2) {
        const std::optional<Operand> case_value = operand_of(at(idx));
        if (!case_value.has_value()) {
          (void)unsupported(span, "a case the backend cannot spell");
          return false;
        }
        push(make_inst(MOp::Cmp, width, *value, *case_value));
        push(make_jcc(
            Cond::E,
            Operand{Label{body_.block_labels[at(idx + 1).as_block().idx]}}));
      }
      push(make_inst(MOp::Jmp, 8,
                     Operand{Label{body_.block_labels[at(1).as_block().idx]}}));
      return true;
    }
    case O::Select: {
      // operands = [cond, true_val, false_val].
      DCHECK(count == 3);
      if (!has_dst) {
        return true;
      }
      const std::optional<Operand> cond = scalar_of(at(0), span);
      const std::optional<Operand> yes = scalar_of(at(1), span);
      const std::optional<Operand> no = scalar_of(at(2), span);
      if (!cond.has_value() || !yes.has_value() || !no.has_value()) {
        return false;
      }
      const u32 taken = fresh_label();
      const u32 done = fresh_label();
      push(make_inst(MOp::Cmp,
                     compute_width_of(storage_.types()[at(0).type].tag), *cond,
                     Operand{Imm{0}}));
      push(make_jcc(Cond::Ne, Operand{Label{taken}}));
      push(make_inst(MOp::Mov, 8, dst_operand(), *no));
      push(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
      MInst yes_move = make_inst(MOp::Mov, 8, dst_operand(), *yes);
      yes_move.label = taken;
      push(yes_move);
      // The join, which carries no code of its own.
      MInst join = make_inst(MOp::Nop, 8);
      join.label = done;
      push(join);
      return true;
    }
    case O::TypeSizeOf:
    case O::TypeAlignOf: {
      if (!has_dst) {
        return true;
      }
      const ir::TypeLayout layout = storage_.layout_of(instr.measure, width_);
      push(make_inst(
          MOp::Mov, 8, dst_operand(),
          Operand{Imm{static_cast<i64>(
              instr.op == O::TypeSizeOf ? layout.size : layout.align)}}));
      return true;
    }
    case O::Alloca: {
      if (!has_dst) {
        return true;
      }
      // The operand is the element count, which every lowering spells as
      // a constant; a slot whose size is a value is not a thing this IR
      // carries.
      // The element count is a constant when it is there at all; a slot
      // whose size is a value is not a thing this IR carries today.
      u64 elements = 1;
      if (!ops.empty()) {
        if (!at(0).is<ir::ImmutableIdx>()) {
          (void)unsupported(span, "a slot whose size is a value");
          return false;
        }
        const ir::Immutable& count_imm =
            storage_.immutables()[at(0).as_immutable()];
        elements =
            count_imm.as_u64_integer(storage_.types()[count_imm.type.idx].tag);
      }
      const ir::TypeLayout layout = storage_.layout_of(dst_type_of(), width_);
      body_.allocas.push_back(Body::Alloca{
          .vreg = vreg_of(instr.dst, dst_type_of()),
          .size = layout.size * elements,
          .align = layout.align,
      });
      note_pointee(instr.dst, dst_type_of());
      return true;
    }
    case O::Load: {
      DCHECK(count == 1);
      if (!has_dst) {
        return true;
      }
      if (!at(0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::TypeLayout layout = storage_.layout_of(dst_type_of(), width_);
      push(make_inst(
          MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
          dst_operand(),
          Operand{Mem{.base = VReg{vreg_of(at(0).as_register(), at(0).type)},
                      .index = std::nullopt,
                      .scale = 1,
                      .disp = 0}}));
      note_pointee(instr.dst, dst_type_of());
      return true;
    }
    case O::Store: {
      // operands = [value, ptr].
      DCHECK(count == 2);
      if (!at(1).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::Operand& value = at(0);
      const ir::TypeTag tag = storage_.types()[value.type].tag;
      const ir::TypeLayout layout = storage_.layout_of(value.type, width_);
      const std::optional<Operand> src = scalar_of(value, span);
      if (!src.has_value()) {
        return false;
      }
      push(make_inst(
          MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
          Operand{Mem{.base = VReg{vreg_of(at(1).as_register(), at(1).type)},
                      .index = std::nullopt,
                      .scale = 1,
                      .disp = 0}},
          *src));
      (void)tag;
      return true;
    }
    case O::GetElementPtr: {
      // operands = [base_ptr, indices...]; the element type comes from
      // the instruction that made the base pointer.
      DCHECK(count >= 2);
      if (!has_dst) {
        return true;
      }
      if (!at(0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::RegisterIdx base = at(0).as_register();
      std::optional<ir::TypeIdx> ty = pointee_hint(base);
      if (!ty.has_value()) {
        (void)unsupported(span, "a projection of an untracked pointer");
        return false;
      }
      i64 offset = 0;
      std::optional<Operand> index;
      u64 scale = 1;
      for (u32 idx = 1; idx < count; ++idx) {
        const std::optional<Operand> step = operand_of(at(idx));
        if (!step.has_value()) {
          (void)unsupported(span, "a projection the backend cannot spell");
          return false;
        }
        if (idx == 1) {
          // The first index steps over the base object itself, which
          // every lowering spells as zero.
          if (const Imm* imm = std::get_if<Imm>(&*step);
              imm != nullptr && imm->value == 0) {
            continue;
          }
          (void)unsupported(span, "a projection that leaves its object");
          return false;
        }
        const ir::TypeTag tag = storage_.types()[ty->idx].tag;
        if (tag == ir::TypeTag::Array) {
          const ir::ArrayType& shape =
              storage_.array_types()[storage_.types()[ty->idx].as_array()];
          const ir::TypeLayout element =
              storage_.layout_of(shape.element, width_);
          if (const Imm* imm = std::get_if<Imm>(&*step); imm != nullptr) {
            offset += static_cast<i64>(element.size) * imm->value;
          } else {
            index = *step;
            scale = element.size;
          }
          ty = shape.element;
          continue;
        }
        if (tag == ir::TypeTag::Struct) {
          if (const Imm* imm = std::get_if<Imm>(&*step); imm == nullptr) {
            (void)unsupported(span, "a dynamic field projection");
            return false;
          } else {
            offset += static_cast<i64>(ir::field_offset(
                storage_.state(),
                storage_.struct_types()[storage_.types()[ty->idx].as_struct()]
                    .fields,
                static_cast<u32>(imm->value), width_));
            ty = storage_.struct_types()[storage_.types()[ty->idx].as_struct()]
                     .fields[static_cast<u32>(imm->value)];
          }
          continue;
        }
        (void)unsupported(span, "a projection into a scalar");
        return false;
      }
      push(make_inst(MOp::Lea, 8, dst_operand(),
                     Operand{Mem{.base = VReg{vreg_of(base, at(0).type)},
                                 .index = regref_of(index),
                                 .scale = static_cast<u8>(scale),
                                 .disp = static_cast<i32>(offset)}}));
      note_pointee(instr.dst, *ty);
      return true;
    }
    case O::ElemOffset: {
      // operands = [base_ptr, index]; the destination's own type names
      // the element.
      DCHECK(count == 2);
      if (!has_dst) {
        return true;
      }
      if (!at(0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::TypeTag pointee_tag = dst_tag();
      if (pointee_tag != ir::TypeTag::Ref &&
          pointee_tag != ir::TypeTag::MutRef) {
        (void)unsupported(span, "an element projection of a non-reference");
        return false;
      }
      const ir::TypeIdx element =
          storage_.ref_types()[storage_.types()[dst_type_of().idx].as_ref()]
              .pointee;
      const ir::TypeLayout layout = storage_.layout_of(element, width_);
      const std::optional<Operand> index = scalar_of(at(1), span);
      if (!index.has_value()) {
        return false;
      }
      push(make_inst(
          MOp::Lea, 8, dst_operand(),
          Operand{Mem{.base = VReg{vreg_of(at(0).as_register(), at(0).type)},
                      .index = regref_of(index),
                      .scale = static_cast<u8>(layout.size),
                      .disp = 0}}));
      note_pointee(instr.dst, element);
      return true;
    }
    case O::Memcopy: {
      // operands = [dst, src, len], and the copy runs on the registers
      // the instruction form names.
      DCHECK(count == 3);
      for (const u32 which : {0u, 1u, 2u}) {
        const std::optional<Operand> side = scalar_of(at(which), span);
        if (!side.has_value()) {
          return false;
        }
        push(make_inst(MOp::Mov, 8,
                       which == 0   ? Operand{Reg::Rdi}
                       : which == 1 ? Operand{Reg::Rsi}
                                    : Operand{Reg::Rcx},
                       *side));
      }
      push(make_inst(MOp::RepMovsb, 8));
      return true;
    }
    case O::Memset: {
      // operands = [dst, value, len].
      DCHECK(count == 3);
      for (const u32 which : {0u, 1u, 2u}) {
        const std::optional<Operand> side = scalar_of(at(which), span);
        if (!side.has_value()) {
          return false;
        }
        push(make_inst(MOp::Mov, which == 1 ? 1 : 8,
                       which == 0   ? Operand{Reg::Rdi}
                       : which == 1 ? Operand{Reg::Rax}
                                    : Operand{Reg::Rcx},
                       *side));
      }
      push(make_inst(MOp::RepStosb, 8));
      return true;
    }
    case O::ExtractValue: {
      // operands = [aggregate, index...].
      DCHECK(count >= 2);
      if (!has_dst) {
        return true;
      }
      if (at(0).is<ir::ImmutableIdx>()) {
        // A literal's fields are constants: the bytes live in the data
        // section and the length is the count of them.
        const ir::Immutable& imm = storage_.immutables()[at(0).as_immutable()];
        const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
        if (tag != ir::TypeTag::Str) {
          (void)unsupported(span,
                            "a literal field the backend cannot "
                            "spell");
          return false;
        }
        if (!at(1).is<ir::ImmutableIdx>()) {
          (void)unsupported(span, "a dynamic field projection");
          return false;
        }
        const ir::Immutable& index_imm =
            storage_.immutables()[at(1).as_immutable()];
        const u64 which =
            index_imm.as_u64_integer(storage_.types()[index_imm.type.idx].tag);
        const SymbolRef symbol =
            string_ref(rodata_.intern(strings_.get(imm.data.str_id_value)));
        if (which == 0) {
          push(make_inst(MOp::Mov, 8, dst_operand(), Operand{symbol}));
        } else {
          push(make_inst(MOp::Mov, 8, dst_operand(),
                         Operand{Imm{static_cast<i64>(
                             strings_.get(imm.data.str_id_value).size())}}));
        }
        return true;
      }
      // A value's field rides in its slot at the offset the layout
      // gives it.
      const ir::RegisterIdx aggregate = at(0).as_register();
      const ir::TypeTag tag = storage_.types()[at(0).type].tag;
      i64 offset = 0;
      if (tag == ir::TypeTag::Str) {
        if (!at(1).is<ir::ImmutableIdx>()) {
          (void)unsupported(span, "a dynamic field projection");
          return false;
        }
        const ir::Immutable& index_imm =
            storage_.immutables()[at(1).as_immutable()];
        const u64 which =
            index_imm.as_u64_integer(storage_.types()[index_imm.type.idx].tag);
        offset = static_cast<i64>(which * 8);
      } else {
        (void)unsupported(span,
                          "a field of an aggregate the backend has "
                          "no layout for yet");
        return false;
      }
      const ir::TypeLayout layout = storage_.layout_of(dst_type_of(), width_);
      push(make_inst(MOp::Mov,
                     static_cast<u8>(layout.size > 8 ? 8 : layout.size),
                     dst_operand(),
                     Operand{Mem{.base = VReg{vreg_of(aggregate, at(0).type)},
                                 .index = std::nullopt,
                                 .scale = 1,
                                 .disp = static_cast<i32>(offset)}}));
      return true;
    }
    case O::InsertValue: {
      (void)unsupported(span, "an aggregate field write");
      return false;
    }
    case O::Call: {
      // operands = [callee, arguments...].
      DCHECK(count >= 1);
      // The name the call reaches: a program's own function, or the one
      // an extern declaration spells.
      std::string symbol;
      if (at(0).is<ir::FunctionIdx>()) {
        symbol = internal_symbol(at(0).as_function());
      } else if (at(0).is<ir::ExternalFunctionIdx>()) {
        symbol = std::string(strings_.get(
            storage_.external_functions()[at(0).as_external_function()]
                .meta.name));
      } else {
        (void)unsupported(span, "a call through a value");
        return false;
      }
      u32 symbol_id = body_.symbols.size();
      for (u32 existing = 0; existing < body_.symbols.size(); ++existing) {
        if (body_.symbols[existing] == symbol) {
          symbol_id = existing;
          break;
        }
      }
      if (symbol_id == body_.symbols.size()) {
        body_.symbols.push_back(symbol);
      }
      for (u32 arg = 1; arg < count; ++arg) {
        const ir::Operand& value = at(arg);
        const ir::TypeTag tag = storage_.types()[value.type].tag;
        if (is_aggregate(tag) || is_two_word(tag)) {
          (void)unsupported(span, "an argument wider than a register");
          return false;
        }
        if (is_float(tag)) {
          (void)unsupported(span, "a floating point argument");
          return false;
        }
        const std::optional<Operand> side = scalar_of(value, span);
        if (!side.has_value()) {
          return false;
        }
        if (arg - 1 < 6) {
          push(make_inst(MOp::Mov, width_of(tag), Operand{ARG_REGS[arg - 1]},
                         *side));
        } else {
          // The rest ride on the stack the callee reads them from,
          // which the frame reserves below its own slots.
          const u32 slot = static_cast<u32>(arg - 7) * 8;
          body_.outgoing = std::max(body_.outgoing, slot + 8);
          push(make_inst(MOp::Mov, 8,
                         Operand{Mem{.base = Reg::Rsp,
                                     .index = std::nullopt,
                                     .scale = 1,
                                     .disp = static_cast<i32>(slot)}},
                         *side));
        }
      }
      push(make_inst(MOp::Call, 8, Operand{SymbolRef{symbol_id}}));
      if (has_dst) {
        const ir::TypeTag tag = dst_tag();
        if (is_aggregate(tag) || is_two_word(tag)) {
          (void)unsupported(span, "a return wider than a register");
          return false;
        }
        push(make_inst(MOp::Mov, width_of(tag), dst_operand(),
                       Operand{Reg::Rax}));
      }
      return true;
    }
    default: break;
  }

  // Everything left reads one or two scalars and writes one.
  if (!has_dst) {
    return true;
  }
  const u8 compute = compute_width_of(dst_tag());
  const std::optional<Operand> lhs = scalar_of(at(0), span);
  if (!lhs.has_value()) {
    return false;
  }

  switch (instr.op) {
    case O::IntAdd:
    case O::IntAddWrap:
    case O::IntSub:
    case O::IntSubWrap:
    case O::And:
    case O::Or:
    case O::Xor: {
      DCHECK(count == 2);
      push(make_inst(MOp::Mov, compute, dst_operand(), *lhs));
      const std::optional<Operand> rhs = scalar_of(at(1), span);
      if (!rhs.has_value()) {
        return false;
      }
      // The opcode carries a 32-bit immediate; a wider constant takes a
      // register the instruction is free to clobber.
      Operand right = *rhs;
      if (const Imm* value = std::get_if<Imm>(&right);
          value != nullptr &&
          (value->value > 0x7fffffffLL || value->value < -0x80000000LL)) {
        push(make_inst(MOp::Mov, compute, Operand{Reg::Rdx}, right));
        right = Operand{Reg::Rdx};
      }
      push(make_inst(alu_of(instr.op), compute, dst_operand(), right));
      return true;
    }
    case O::IntMul:
    case O::IntMulWrap: {
      DCHECK(count == 2);
      push(make_inst(MOp::Mov, compute, dst_operand(), *lhs));
      const std::optional<Operand> rhs = scalar_of(at(1), span);
      if (!rhs.has_value()) {
        return false;
      }
      if (const Imm* immediate = std::get_if<Imm>(&*rhs);
          immediate != nullptr) {
        // The two-operand form reads a register, so a constant takes one
        // the instruction is free to clobber.
        push(make_inst(MOp::Mov, compute, Operand{Reg::Rcx}, *rhs));
        push(make_inst(MOp::Imul, compute, dst_operand(), Operand{Reg::Rcx}));
      } else {
        push(make_inst(MOp::Imul, compute, dst_operand(), *rhs));
      }
      return true;
    }
    case O::IntDiv:
    case O::IntRem:
    case O::UintDiv:
    case O::UintRem: {
      DCHECK(count == 2);
      // The dividend rides in `rax` and its sign in `rdx`, which is what
      // the one-operand form reads them from.
      push(make_inst(MOp::Mov, compute, Operand{Reg::Rax}, *lhs));
      if (is_signed(dst_tag())) {
        push(make_inst(MOp::Cqo, compute));
      } else {
        push(
            make_inst(MOp::Xor, compute, Operand{Reg::Rdx}, Operand{Reg::Rdx}));
      }
      const std::optional<Operand> rhs = scalar_of(at(1), span);
      if (!rhs.has_value()) {
        return false;
      }
      push(make_inst(instr.op == O::IntDiv || instr.op == O::IntRem ? MOp::Idiv
                                                                    : MOp::Udiv,
                     compute, *rhs));
      push(make_inst(MOp::Mov, compute, dst_operand(),
                     instr.op == O::IntDiv || instr.op == O::UintDiv
                         ? Operand{Reg::Rax}
                         : Operand{Reg::Rdx}));
      return true;
    }
    case O::ShiftLeft:
    case O::ArithmeticShiftRight:
    case O::LogicalShiftRight: {
      DCHECK(count == 2);
      push(make_inst(MOp::Mov, compute, dst_operand(), *lhs));
      const std::optional<Operand> rhs = scalar_of(at(1), span);
      if (!rhs.has_value()) {
        return false;
      }
      MOp op = shift_mop_of(instr.op);
      // A count in a register has to reach `cl`, which is the only
      // register a shift reads it from; a constant rides in the
      // instruction.
      if (const Imm* imm = std::get_if<Imm>(&*rhs); imm != nullptr) {
        push(make_inst(op, compute, dst_operand(), *rhs));
      } else {
        push(make_inst(MOp::Mov, 1, Operand{Reg::Rcx}, *rhs));
        push(make_inst(op, compute, dst_operand(), Operand{Reg::Rcx}));
      }
      return true;
    }
    case O::Not: {
      DCHECK(count == 1);
      push(make_inst(MOp::Mov, compute, dst_operand(), *lhs));
      if (dst_tag() == ir::TypeTag::I1) {
        // A bool's negation is logical, and a bool rides as zero or
        // one, so flipping the one bit is the whole answer.
        push(make_inst(MOp::Xor, compute, dst_operand(), Operand{Imm{1}}));
      } else {
        push(make_inst(MOp::Not, compute, dst_operand()));
      }
      return true;
    }
    case O::BitReverse: {
      (void)unsupported(span, "a bit reversal");
      return false;
    }
    case O::Eq:
    case O::Ne:
    case O::Le:
    case O::Lt:
    case O::Ge:
    case O::Gt: {
      DCHECK(count == 2);
      const std::optional<Operand> rhs = scalar_of(at(1), span);
      if (!rhs.has_value()) {
        return false;
      }
      // The compare reads its left side from a register, and its right
      // side from one when the constant is wider than the opcode's
      // immediate carries. Both take registers the instruction is free
      // to clobber.
      Operand left = *lhs;
      Operand right = *rhs;
      const bool left_constant = std::holds_alternative<Imm>(left);
      if (left_constant) {
        push(make_inst(MOp::Mov, compute, Operand{Reg::Rcx}, left));
        left = Operand{Reg::Rcx};
      }
      if (std::holds_alternative<Imm>(right)) {
        const Imm& value = std::get<Imm>(right);
        const bool fits =
            value.value >= -0x80000000LL && value.value <= 0x7fffffffLL;
        if (left_constant || !fits) {
          push(make_inst(MOp::Mov, compute, Operand{Reg::Rdx}, right));
          right = Operand{Reg::Rdx};
        }
      }
      push(make_inst(MOp::Cmp, compute, left, right));
      push(make_setcc(compare_of(instr.op, is_signed(dst_tag())),
                      dst_operand()));
      return true;
    }
    case O::TypeCast: {
      DCHECK(count == 1);
      const ir::TypeTag src = storage_.types()[at(0).type].tag;
      if (is_float(src) || is_float(dst_tag())) {
        (void)unsupported(span, "a floating point conversion");
        return false;
      }
      if (is_integer(src) && is_integer(dst_tag())) {
        const u8 from = width_of(src);
        const u8 to = width_of(dst_tag());
        if (from < to) {
          // A wider value reads the narrow one's own bytes and
          // re-derives the rest.
          push(make_inst(is_signed(src) ? MOp::Movsx : MOp::Movzx, from,
                         dst_operand(), *lhs));
        } else {
          push(make_inst(MOp::Mov, 8, dst_operand(), *lhs));
        }
        return true;
      }
      // A pointer and an integer of the machine's width are the same
      // bytes; two pointers are too.
      push(make_inst(MOp::Mov, 8, dst_operand(), *lhs));
      return true;
    }
    default: {
      (void)unsupported(span,
                        "an instruction the backend has no lowering "
                        "for");
      return false;
    }
  }
}

}  // namespace

base::Result<Body, codegen::EmitError> select_function(
    const ir::Storage& storage,
    const ir::SymbolTable& strings,
    std::span<const diag::Span> spans,
    const ir::Function& function,
    diag::DiagBag& bag,
    const ir::PointerWidth width,
    RoData& rodata) {
  Selector selector(storage, strings, spans, function, bag, width, rodata);
  return selector.run();
}

}  // namespace codegen::x86_64
