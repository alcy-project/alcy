// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/select.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
// nothing else. The pointer-width spellings ride at the word either
// way, which is what the layout says of them.
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
  return tag == ir::TypeTag::I8 || tag == ir::TypeTag::I16 ||
         tag == ir::TypeTag::I32 || tag == ir::TypeTag::I64 ||
         tag == ir::TypeTag::Isize;
}

constexpr bool is_integer(const ir::TypeTag tag) {
  // A bool rides as zero or one, which is what an integer immediate
  // spells; the compare reads it unsigned for the same reason.
  return is_signed(tag) || tag == ir::TypeTag::I1 ||
         tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
         tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64 ||
         tag == ir::TypeTag::Usize;
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

  // The enum opcodes, which read a place rather than a value, and the
  // call, which marshals every shape the ABI names.
  [[nodiscard]] bool emit_enum(const ir::Instruction& instr,
                               const ir::OperandIdxRange ops,
                               const diag::Span span);
  [[nodiscard]] bool emit_call(const ir::Instruction& instr,
                               const ir::OperandIdxRange ops,
                               const diag::Span span);

  [[nodiscard]] static bool defines_dst(const ir::Instruction& instr) {
    return instr.dst.is_valid();
  }
  [[nodiscard]] ir::TypeIdx dst_type(const ir::Instruction& instr) const {
    return storage_.registers()[instr.dst].type;
  }
  [[nodiscard]] Operand dst_reg_operand(const ir::Instruction& instr) {
    return Operand{VReg{vreg_of(instr.dst, dst_type(instr))}};
  }
  [[nodiscard]] ir::TypeTag dst_value_tag(const ir::Instruction& instr) const {
    return storage_.types()[dst_type(instr).idx].tag;
  }
  [[nodiscard]] u32 fresh_label() { return body_.label_count++; }
  // One instruction's operand, by position.
  [[nodiscard]] const ir::Operand& operand_at(const ir::Instruction& instr,
                                              const u32 index) const {
    return storage_.operands()[instr.operands.head() + index];
  }

  // Appends one instruction, carrying the block's label on the first one
  // the block lowers to.
  void push(MInst inst);

  // Brings the function's own parameters in from where the ABI put
  // them, at the head of the entry block. A wide answer takes the
  // caller's slot first, which is the pointer the first register held.
  [[nodiscard]] bool receive_parameters(const ir::Block& entry);

  // The type one parameter rides in, which is what decides how it
  // crosses a call.
  enum class Passage : u8 { Register, TwoWord, ByAddress };

  [[nodiscard]] Passage passage_of(ir::TypeIdx type) const;

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

  void note_alloca(ir::RegisterIdx reg);

  // The symbol reference one string's bytes answer to, in this body's
  // own table.
  [[nodiscard]] SymbolRef string_ref(const std::string& symbol);

  // The memory reference one place reads, at the offset from it. A
  // register an `alloca` named names its own storage; any other holds
  // the address, and the reference goes through it.
  [[nodiscard]] Operand place_of(const ir::Operand& place, i32 offset);

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
  // The vregs an `alloca` named, whose slot is the storage itself. A
  // projection's destination is not one: it holds the address, and a
  // read through it dereferences like any pointer.
  std::unordered_set<u32> alloca_storage_;
  // The vreg that holds the caller's return slot, when the answer is
  // wider than two words; `NO_SRET` when the answer rides in registers.
  static constexpr u32 NO_SRET = 0xffffffff;
  u32 sret_vreg_ = NO_SRET;
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

void Selector::note_alloca(const ir::RegisterIdx reg) {
  alloca_storage_.insert(vreg_of(reg, storage_.registers()[reg].type));
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

Operand Selector::place_of(const ir::Operand& place, const i32 offset) {
  const u32 vreg = vreg_of(place.as_register(), place.type);
  if (alloca_storage_.count(vreg) != 0) {
    return Operand{SlotRef{vreg, offset}};
  }
  return Operand{Mem{
      .base = VReg{vreg}, .index = std::nullopt, .scale = 1, .disp = offset}};
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
  if (passage_of(op.type) != Passage::Register) {
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
  if (op.is<ir::RegisterIdx>()) {
    const ir::TypeTag tag = storage_.types()[op.type].tag;
    if (passage_of(op.type) == Passage::TwoWord) {
      // A two-word value the body reads as two words: the pair rides in
      // its own slot, which is the reference a scalar read mistakes for
      // one word.
      (void)unsupported(span, "a two-word read");
      return std::nullopt;
    }
    (void)tag;
  }
  const char* kind = "other";
  if (op.is<ir::ImmutableIdx>()) {
    kind = "imm";
  } else if (op.is<ir::RegisterIdx>()) {
    kind = "reg";
  } else if (op.is<ir::BlockIdx>()) {
    kind = "block";
  }
  (void)unsupported(span, kind);
  return std::nullopt;
}

base::Result<Body, codegen::EmitError> Selector::run() {
  // A function whose answer is wider than two words takes the slot the
  // caller named: one hidden vreg holds the pointer, and the return
  // stores through it.
  const ir::TypeTag returned =
      storage_.types()[function_.meta.return_type.idx].tag;
  if (is_aggregate(returned)) {
    sret_vreg_ = static_cast<u32>(body_.vreg_types.size());
    body_.vreg_types.push_back(ir::primitive_idx(ir::TypeTag::Ptr).idx);
  }

  // Every block the body carries gets a label, whether or not a branch
  // names it: a block's first instruction is where its label sits, and
  // the map from block to label is what the branches read.
  bool entry = true;
  for (const ir::BlockIdx block_idx : function_.blocks) {
    body_.block_labels.emplace(block_idx.idx, body_.label_count++);
  }
  for (const ir::BlockIdx block_idx : function_.blocks) {
    label_ = body_.block_labels[block_idx.idx];
    placed_ = false;
    const ir::Block& block = storage_.blocks()[block_idx];
    if (entry) {
      entry = false;
      // The entry block's parameters are the function's own, and they
      // arrive where the ABI put them. Every one moves to the slot its
      // vreg holds, which is where the body reads it from.
      if (!receive_parameters(block)) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
    }
    for (const ir::InstructionIdx instr_idx : block.instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      if (!emit(instr, span_of(instr_idx))) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
    }
    if (!placed_) {
      // A block with no instructions still needs its label to name a
      // byte, or a branch to it lands at offset zero.
      push(make_inst(MOp::Nop, 8));
    }
  }
  if (failed_) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  return base::make_ok(std::move(body_));
}

void Selector::push(const MInst inst) {
  MInst lowered = inst;
  if (!placed_) {
    // The label rides on the first instruction the block lowers to,
    // which is what makes a jump land on its own materialization.
    lowered.label = label_;
    placed_ = true;
  }
  body_.insts.push_back(lowered);
}

Selector::Passage Selector::passage_of(const ir::TypeIdx type) const {
  const ir::TypeTag tag = storage_.types()[type.idx].tag;
  if (is_aggregate(tag)) {
    return Passage::ByAddress;
  }
  // A reference to a slice is the fat pointer the callers handle beside
  // these: two words, whatever the reference's own tag reads.
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    const ir::TypeIdx pointee =
        storage_.ref_types()[storage_.types()[type.idx].as_ref()].pointee;
    if (storage_.types()[pointee.idx].tag == ir::TypeTag::Slice) {
      return Passage::TwoWord;
    }
    return Passage::Register;
  }
  if (is_two_word(tag)) {
    return Passage::TwoWord;
  }
  return Passage::Register;
}

bool Selector::receive_parameters(const ir::Block& entry) {
  // The argument registers the call filled, in the order the
  // declaration names its parameters. A wide answer took the first one
  // for the caller's slot.
  u32 next_register = 0;
  if (sret_vreg_ != NO_SRET) {
    push(make_inst(MOp::Mov, 8, Operand{SlotRef{sret_vreg_, 0}},
                   Operand{ARG_REGS[next_register++]}));
  }
  for (const ir::BlockParamIdx param_idx : entry.block_params) {
    const ir::BlockParam& param = storage_.block_params()[param_idx];
    const u32 vreg = vreg_of(param.reg, param.type);
    const ir::TypeLayout layout = storage_.layout_of(param.type, width_);
    switch (passage_of(param.type)) {
      case Passage::Register: {
        if (next_register >= ARG_REG_COUNT) {
          (void)unsupported(diag::Span{},
                            "a parameter past the argument "
                            "registers");
          return false;
        }
        const ir::TypeTag tag = storage_.types()[param.type.idx].tag;
        push(make_inst(MOp::Mov, width_of(tag), Operand{VReg{vreg}},
                       Operand{ARG_REGS[next_register++]}));
        break;
      }
      case Passage::TwoWord: {
        if (next_register + 1 >= ARG_REG_COUNT) {
          (void)unsupported(diag::Span{},
                            "a parameter past the argument "
                            "registers");
          return false;
        }
        push(make_inst(MOp::Mov, 8, Operand{SlotRef{vreg, 0}},
                       Operand{ARG_REGS[next_register++]}));
        push(make_inst(MOp::Mov, 8, Operand{SlotRef{vreg, 8}},
                       Operand{ARG_REGS[next_register++]}));
        break;
      }
      case Passage::ByAddress: {
        if (next_register >= ARG_REG_COUNT) {
          (void)unsupported(diag::Span{},
                            "a parameter past the argument "
                            "registers");
          return false;
        }
        // The caller hands the parameter's bytes over by address, and
        // the slot is where this body reads them from, so the copy is
        // the whole of receiving it. The byte copy borrows `rdi`, `rsi`
        // and `rcx`, which may still carry arguments for later
        // parameters; hold its address in `rax` and save the three it
        // clobbers around the copy.
        push(make_inst(MOp::Mov, 8, Operand{Reg::Rax},
                       Operand{ARG_REGS[next_register++]}));
        push(make_inst(MOp::Push, 8, Operand{Reg::Rdi}));
        push(make_inst(MOp::Push, 8, Operand{Reg::Rsi}));
        push(make_inst(MOp::Push, 8, Operand{Reg::Rcx}));
        push(make_inst(MOp::Mov, 8, Operand{Reg::Rsi}, Operand{Reg::Rax}));
        push(make_inst(MOp::Lea, 8, Operand{Reg::Rdi},
                       Operand{SlotRef{vreg, 0}}));
        push(make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                       Operand{Imm{static_cast<i64>(layout.size)}}));
        push(make_inst(MOp::RepMovsb, 8));
        push(make_inst(MOp::Pop, 8, Operand{Reg::Rcx}));
        push(make_inst(MOp::Pop, 8, Operand{Reg::Rsi}));
        push(make_inst(MOp::Pop, 8, Operand{Reg::Rdi}));
        break;
      }
    }
  }
  return true;
}

bool Selector::emit(const ir::Instruction& instr, const diag::Span span) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const usize count = ops.size();
  const auto at = [&](const u32 index) { return operand_at(instr, index); };

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
          const Passage passage = passage_of(value.type);
          if (passage == Passage::ByAddress) {
            // The caller named the slot; the bytes move there whole.
            if (!value.is<ir::RegisterIdx>()) {
              (void)unsupported(span,
                                "a constant return the backend cannot "
                                "spell");
              return false;
            }
            if (sret_vreg_ == NO_SRET) {
              (void)unsupported(span, "a wide answer with no slot");
              return false;
            }
            push(make_inst(MOp::Mov, 8, Operand{Reg::Rdi},
                           Operand{SlotRef{sret_vreg_, 0}}));
            push(make_inst(
                MOp::Lea, 8, Operand{Reg::Rsi},
                Operand{SlotRef{vreg_of(value.as_register(), value.type), 0}}));
            push(make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                           Operand{Imm{static_cast<i64>(layout.size)}}));
            push(make_inst(MOp::RepMovsb, 8));
          } else if (value.is<ir::RegisterIdx>()) {
            const u32 vreg = vreg_of(value.as_register(), value.type);
            // One word answers in `rax`; two ride in the pair.
            push(
                make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{VReg{vreg}}));
            if (passage == Passage::TwoWord) {
              push(make_inst(MOp::Mov, 8, Operand{Reg::Rdx},
                             Operand{SlotRef{vreg, 8}}));
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
      note_alloca(instr.dst);
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
      const ir::TypeTag loaded = dst_tag();
      // A value wider than one register crosses as its whole slot. The
      // allocator turns this into the byte copy the destination needs;
      // spelling the halves as separate moves would ask the encoder for
      // a memory-to-memory move x86 does not have.
      const ir::TypeLayout layout = storage_.layout_of(dst_type_of(), width_);
      if (is_two_word(loaded) || is_aggregate(loaded)) {
        const u32 vreg = vreg_of(instr.dst, dst_type_of());
        push(make_inst(
            MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
            Operand{VReg{vreg}},
            Operand{Mem{.base = VReg{vreg_of(at(0).as_register(), at(0).type)},
                        .index = std::nullopt,
                        .scale = 1,
                        .disp = 0}}));
        return true;
      }
      push(make_inst(
          MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
          dst_operand(),
          Operand{Mem{.base = VReg{vreg_of(at(0).as_register(), at(0).type)},
                      .index = std::nullopt,
                      .scale = 1,
                      .disp = 0}}));
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
      if (is_aggregate(tag) || is_two_word(tag)) {
        if (!value.is<ir::RegisterIdx>()) {
          (void)unsupported(span, "a constant aggregate store");
          return false;
        }
        // An aggregate crosses as the bytes its slot holds, which is
        // the whole of what the store has to say. A narrow aggregate
        // must not write a whole word past its slot.
        push(make_inst(
            MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
            Operand{Mem{.base = VReg{vreg_of(at(1).as_register(), at(1).type)},
                        .index = std::nullopt,
                        .scale = 1,
                        .disp = 0}},
            Operand{VReg{vreg_of(value.as_register(), value.type)}}));
        return true;
      }
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
      // A projection's destination names the element it reaches, and
      // the register holds its address, so the vreg is a pointer's
      // width whatever the element is.
      const u32 address =
          vreg_of(instr.dst, ir::primitive_idx(ir::TypeTag::Ptr));
      push(make_inst(MOp::Lea, 8, Operand{VReg{address}},
                     Operand{Mem{.base = VReg{vreg_of(base, at(0).type)},
                                 .index = regref_of(index),
                                 .scale = static_cast<u8>(scale),
                                 .disp = static_cast<i32>(offset)}}));
      note_pointee(instr.dst, *ty);
      return true;
    }
    case O::EnumTagLoad:
    case O::EnumTagSet:
    case O::EnumFieldAddr: return emit_enum(instr, ops, span);
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
      // A constant element index folds into the displacement; a register
      // one rides through the scaled-index field.
      i64 disp = 0;
      std::optional<RegRef> scaled;
      if (const Imm* imm = std::get_if<Imm>(&*index); imm != nullptr) {
        disp = imm->value * static_cast<i64>(layout.size);
      } else {
        scaled = regref_of(index);
        if (!scaled.has_value()) {
          (void)unsupported(span, "an element offset the backend cannot spell");
          return false;
        }
      }
      push(make_inst(
          MOp::Lea, 8, dst_operand(),
          Operand{Mem{.base = VReg{vreg_of(at(0).as_register(), at(0).type)},
                      .index = scaled,
                      .scale = static_cast<u8>(layout.size),
                      .disp = static_cast<i32>(disp)}}));
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
      // gives it, which the aggregate's own field walk publishes.
      if (!at(0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      if (!at(1).is<ir::ImmutableIdx>()) {
        (void)unsupported(span, "a dynamic field projection");
        return false;
      }
      const ir::RegisterIdx aggregate = at(0).as_register();
      const ir::TypeIdx aggregate_type = at(0).type;
      const ir::Immutable& index_imm =
          storage_.immutables()[at(1).as_immutable()];
      const u64 which =
          index_imm.as_u64_integer(storage_.types()[index_imm.type.idx].tag);
      i64 offset = 0;
      const ir::TypeTag tag = storage_.types()[aggregate_type.idx].tag;
      if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Tuple) {
        const ir::TypeIdxRange fields =
            tag == ir::TypeTag::Struct
                ? storage_.struct_types()
                      [storage_.types()[aggregate_type.idx].as_struct()]
                      .fields
                : storage_
                      .tuple_types()
                      [storage_.types()[aggregate_type.idx].as_tuple()]
                      .elements;
        offset = static_cast<i64>(ir::field_offset(
            storage_.state(), fields, static_cast<u32>(which), width_));
      } else if (tag == ir::TypeTag::Str) {
        // The pair the reference carries: the bytes, then the length.
        offset = static_cast<i64>(which * 8);
      } else {
        (void)unsupported(span,
                          "a field of an aggregate the backend has "
                          "no layout for yet");
        return false;
      }
      const ir::TypeLayout layout = storage_.layout_of(dst_type_of(), width_);
      // The pair rides in the register's own slot: reading a field is
      // the slot reference, not a dereference of the word it holds.
      push(make_inst(MOp::Mov,
                     static_cast<u8>(layout.size > 8 ? 8 : layout.size),
                     dst_operand(),
                     Operand{SlotRef{vreg_of(aggregate, at(0).type),
                                     static_cast<i32>(offset)}}));
      return true;
    }
    case O::InsertValue: {
      (void)unsupported(span, "an aggregate field write");
      return false;
    }
    case O::Call: return emit_call(instr, ops, span);
    default: break;
  }

  // Everything left reads one or two scalars and writes one.
  if (!defines_dst(instr)) {
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
      // The compare runs at its operands' width, which is the width the
      // value holds and not the bool it answers.
      const u8 compare = compute_width_of(storage_.types()[at(0).type].tag);
      // The compare reads its left side from a register, and its right
      // side from one when the constant is wider than the opcode's
      // immediate carries. Both take registers the instruction is free
      // to clobber.
      Operand left = *lhs;
      Operand right = *rhs;
      const bool left_constant = std::holds_alternative<Imm>(left);
      if (left_constant) {
        push(make_inst(MOp::Mov, compare, Operand{Reg::Rcx}, left));
        left = Operand{Reg::Rcx};
      }
      if (std::holds_alternative<Imm>(right)) {
        const Imm& value = std::get<Imm>(right);
        const bool fits =
            value.value >= -0x80000000LL && value.value <= 0x7fffffffLL;
        if (left_constant || !fits) {
          push(make_inst(MOp::Mov, compare, Operand{Reg::Rdx}, right));
          right = Operand{Reg::Rdx};
        }
      }
      push(make_inst(MOp::Cmp, compare, left, right));
      push(make_setcc(
          compare_of(instr.op, is_signed(storage_.types()[at(0).type].tag)),
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

// The enum opcodes. Each reads a place, which is either the storage
// a vreg names or the memory the pointer it holds addresses.
// The enum opcodes. Each reads a place, which is either the storage a
// vreg names or the memory the pointer it holds addresses.
bool Selector::emit_enum(const ir::Instruction& instr,
                         const ir::OperandIdxRange ops,
                         const diag::Span span) {
  const usize count = ops.size();

  if (instr.op == ir::Opcode::EnumTagLoad) {
    // operands = [place]. The representation is the enum's: a niche
    // synthesizes the discriminant from the payload's spare pattern,
    // and a plain enum reads the tag word at its front.
    DCHECK(count == 1);
    if (!defines_dst(instr)) {
      return true;
    }
    const ir::TypeIdx enum_type =
        ir::enum_place_type(storage_.state(), operand_at(instr, 0).type);
    const ir::EnumNiche niche = ir::enum_niche(storage_.state(), enum_type);
    const Operand place = place_of(operand_at(instr, 0), 0);
    if (!niche.has) {
      push(make_inst(MOp::Mov, 4, dst_reg_operand(instr), place));
      return true;
    }
    // The payload's spare pattern spells the fieldless variant: a null
    // where the payload is a reference, the sentinel byte where it is
    // not. Whatever the slot reads, the discriminant is one added when
    // the pattern is the fieldless one.
    const u32 vreg = vreg_of(instr.dst, dst_type(instr));
    const Operand from = place;
    const u8 width = width_of(storage_.types()[niche.slot.idx].tag);
    if (niche.slot == niche.payload) {
      // A reference payload spells null.
      push(make_inst(MOp::Mov, 8, Operand{Reg::Rcx}, from));
      push(make_inst(MOp::Cmp, 8, Operand{Reg::Rcx}, Operand{Imm{0}}));
      push(make_setcc(Cond::E, Operand{Reg::Rax}));
    } else {
      push(make_inst(MOp::Movzx, width, Operand{Reg::Rcx}, from));
      push(make_inst(MOp::Cmp, width, Operand{Reg::Rcx},
                     Operand{Imm{niche.sentinel}}));
      push(make_setcc(Cond::Ae, Operand{Reg::Rax}));
    }
    push(make_inst(MOp::Movzx, 1, Operand{Reg::Rcx}, Operand{Reg::Rax}));
    push(make_inst(MOp::Add, 4, Operand{Reg::Rcx},
                   Operand{Imm{static_cast<i64>(niche.carrying)}}));
    push(make_inst(MOp::Mov, 4, Operand{VReg{vreg}}, Operand{Reg::Rcx}));
    return true;
  }

  if (instr.op == ir::Opcode::EnumTagSet) {
    // operands = [place, tag]. A niche writes its fieldless spell for
    // the fieldless variant; the carrying variant's payload follows, so
    // a zero stands where that payload's store lands.
    DCHECK(count == 2);
    const ir::TypeIdx enum_type =
        ir::enum_place_type(storage_.state(), operand_at(instr, 0).type);
    const ir::EnumNiche niche = ir::enum_niche(storage_.state(), enum_type);
    const Operand place = place_of(operand_at(instr, 0), 0);
    if (!niche.has) {
      const std::optional<Operand> tag = scalar_of(operand_at(instr, 1), span);
      if (!tag.has_value()) {
        return false;
      }
      push(make_inst(MOp::Mov, 4, place, *tag));
      return true;
    }
    const std::optional<Operand> tag = scalar_of(operand_at(instr, 1), span);
    if (!tag.has_value()) {
      return false;
    }
    const u8 width = width_of(storage_.types()[niche.slot.idx].tag);
    const i64 spell =
        niche.slot == niche.payload ? 0 : static_cast<i64>(niche.sentinel);
    const u32 fieldless = fresh_label();
    const u32 done = fresh_label();
    push(make_inst(MOp::Cmp, 4, *tag,
                   Operand{Imm{static_cast<i64>(niche.fieldless)}}));
    push(make_jcc(Cond::Ne, Operand{Label{fieldless}}));
    push(make_inst(MOp::Mov, width, place, Operand{Imm{spell}}));
    push(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
    MInst carrying = make_inst(MOp::Mov, width, place, Operand{Imm{0}});
    carrying.label = fieldless;
    push(carrying);
    MInst join = make_inst(MOp::Nop, 8);
    join.label = done;
    push(join);
    return true;
  }

  // operands = [place, variant, field]. A niche's payload is the slot
  // itself; a plain enum's field lies in the area after the tag, at the
  // offset the variant's fields publish.
  DCHECK(count == 3);
  if (!defines_dst(instr)) {
    return true;
  }
  const ir::TypeIdx enum_type =
      ir::enum_place_type(storage_.state(), operand_at(instr, 0).type);
  const ir::EnumNiche niche = ir::enum_niche(storage_.state(), enum_type);
  const Operand place = place_of(operand_at(instr, 0), 0);
  const u32 address = vreg_of(instr.dst, ir::primitive_idx(ir::TypeTag::Ptr));
  if (niche.has) {
    push(make_inst(MOp::Lea, 8, Operand{VReg{address}}, place));
    note_pointee(instr.dst, niche.payload);
    return true;
  }
  const ir::Immutable& variant_imm =
      storage_.immutables()[operand_at(instr, 1).as_immutable()];
  const u64 variant =
      variant_imm.as_u64_integer(storage_.types()[variant_imm.type.idx].tag);
  const ir::Immutable& field_imm =
      storage_.immutables()[operand_at(instr, 2).as_immutable()];
  const u64 field =
      field_imm.as_u64_integer(storage_.types()[field_imm.type.idx].tag);
  const ir::EnumType& shape =
      storage_.enum_types()[storage_.types()[enum_type.idx].as_enum()];
  const ir::TypeIdxRange fields =
      storage_.enum_variant_types()[shape.variants[variant]].fields;
  // The tag takes the front, and the payload area starts where its own
  // alignment allows.
  const ir::TypeLayout area =
      ir::enum_payload_area(storage_.state(), enum_type, width_);
  const u64 payload_start =
      area.align <= 1 ? 4 : (4 + area.align - 1) / area.align * area.align;
  const i64 offset = static_cast<i64>(
      payload_start + ir::field_offset(storage_.state(), fields,
                                       static_cast<u32>(field), width_));
  push(make_inst(MOp::Lea, 8, Operand{VReg{address}},
                 place_of(operand_at(instr, 0), offset)));
  note_pointee(instr.dst, fields[field]);
  return true;
}

// The call. Every argument crosses the way its type rides, and the
// answer lands where the ABI put it.
bool Selector::emit_call(const ir::Instruction& instr,
                         const ir::OperandIdxRange ops,
                         const diag::Span span) {
  const usize count = ops.size();

  // operands = [callee, arguments...]. A callee this body names by
  // symbol takes a direct call; one it holds in a register takes an
  // indirect one through the code word.
  const bool indirect = operand_at(instr, 0).is<ir::RegisterIdx>();
  u32 symbol_id = 0;
  if (!indirect) {
    std::string symbol;
    if (operand_at(instr, 0).is<ir::FunctionIdx>()) {
      symbol = internal_symbol(operand_at(instr, 0).as_function());
    } else if (operand_at(instr, 0).is<ir::ExternalFunctionIdx>()) {
      symbol = std::string(strings_.get(
          storage_
              .external_functions()[operand_at(instr, 0).as_external_function()]
              .meta.name));
    } else {
      (void)unsupported(span, "a call the backend cannot name");
      return false;
    }
    symbol_id = body_.symbols.size();
    for (u32 existing = 0; existing < body_.symbols.size(); ++existing) {
      if (body_.symbols[existing] == symbol) {
        symbol_id = existing;
        break;
      }
    }
    if (symbol_id == body_.symbols.size()) {
      body_.symbols.push_back(symbol);
    }
  }

  // The argument registers, in the order the call fills them. A
  // wide answer took the first for the caller's slot.
  u32 next_register = 0;
  if (defines_dst(instr) && passage_of(dst_type(instr)) == Passage::ByAddress) {
    // The answer lands in the slot the destination vreg holds, and
    // the callee writes it there; nothing reads `rax`.
    push(make_inst(MOp::Lea, 8, Operand{ARG_REGS[next_register++]},
                   Operand{SlotRef{vreg_of(instr.dst, dst_type(instr)), 0}}));
  }
  for (u32 arg = 1; arg < count; ++arg) {
    const ir::Operand& value = operand_at(instr, arg);
    const ir::TypeTag tag = storage_.types()[value.type].tag;
    if (is_float(tag)) {
      (void)unsupported(span, "a floating point argument");
      return false;
    }
    const Passage passage = passage_of(value.type);
    if (passage != Passage::Register) {
      if (passage == Passage::ByAddress) {
        if (next_register >= ARG_REG_COUNT) {
          (void)unsupported(span,
                            "a parameter past the argument "
                            "registers");
          return false;
        }
        // The bytes cross by address: the slot the value's vreg holds
        // is the copy the callee reads.
        if (!value.is<ir::RegisterIdx>()) {
          (void)unsupported(span, "a constant aggregate argument");
          return false;
        }
        push(make_inst(
            MOp::Lea, 8, Operand{ARG_REGS[next_register++]},
            Operand{SlotRef{vreg_of(value.as_register(), value.type), 0}}));
        continue;
      }
      // A two-word value rides in two registers, which is the pair its
      // slot holds. A literal pair the body carries is the same two
      // words: the bytes the data section holds, and the count of them.
      if (next_register + 1 >= ARG_REG_COUNT) {
        (void)unsupported(span,
                          "a parameter past the argument "
                          "registers");
        return false;
      }
      if (!value.is<ir::RegisterIdx>()) {
        const ir::Immutable& imm =
            storage_.immutables()[value.as_immutable()];
        const SymbolRef bytes = string_ref(
            rodata_.intern(strings_.get(imm.data.str_id_value)));
        push(make_inst(MOp::Mov, 8, Operand{ARG_REGS[next_register++]},
                       Operand{bytes}));
        push(make_inst(MOp::Mov, 8, Operand{ARG_REGS[next_register++]},
                       Operand{Imm{static_cast<i64>(
                           strings_.get(imm.data.str_id_value).size())}}));
        continue;
      }
      const u32 vreg = vreg_of(value.as_register(), value.type);
      push(make_inst(MOp::Mov, 8, Operand{ARG_REGS[next_register++]},
                     Operand{SlotRef{vreg, 0}}));
      push(make_inst(MOp::Mov, 8, Operand{ARG_REGS[next_register++]},
                     Operand{SlotRef{vreg, 8}}));
      continue;
    }
    const std::optional<Operand> side = scalar_of(value, span);
    if (!side.has_value()) {
      return false;
    }
    if (next_register >= ARG_REG_COUNT) {
      (void)unsupported(span, "a parameter past the argument registers");
      return false;
    }
    push(make_inst(MOp::Mov, width_of(tag), Operand{ARG_REGS[next_register++]},
                   *side));
  }
  if (indirect) {
    // A function value is the code and the environment it owns; the
    // call goes through the code word, and the environment rides
    // ahead of every argument.
    const u32 vreg =
        vreg_of(operand_at(instr, 0).as_register(), operand_at(instr, 0).type);
    push(make_inst(MOp::Mov, 8, Operand{ARG_REGS[next_register++]},
                   Operand{SlotRef{vreg, 8}}));
    push(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{SlotRef{vreg, 0}}));
    push(make_inst(MOp::CallReg, 8, Operand{Reg::Rax}));
  } else {
    push(make_inst(MOp::Call, 8, Operand{SymbolRef{symbol_id}}));
  }
  if (defines_dst(instr)) {
    const Passage passage = passage_of(dst_type(instr));
    if (passage == Passage::Register) {
      push(make_inst(MOp::Mov, width_of(dst_value_tag(instr)),
                     dst_reg_operand(instr), Operand{Reg::Rax}));
    } else if (passage == Passage::TwoWord) {
      const u32 vreg = vreg_of(instr.dst, dst_type(instr));
      push(
          make_inst(MOp::Mov, 8, Operand{SlotRef{vreg, 0}}, Operand{Reg::Rax}));
      push(
          make_inst(MOp::Mov, 8, Operand{SlotRef{vreg, 8}}, Operand{Reg::Rdx}));
    }
    // By address, the callee already wrote the slot.
  }
  return true;
}

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
