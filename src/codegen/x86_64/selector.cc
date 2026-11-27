// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/selector.h"

#include <algorithm>
#include <cstdint>
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
#include "overlay/overlay.h"

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
    case ir::TypeTag::U8:
    case ir::TypeTag::Char: return 1;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 2;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32:
    case ir::TypeTag::Rune:
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
  return is_signed(tag) || tag == ir::TypeTag::I1 || tag == ir::TypeTag::U8 ||
         tag == ir::TypeTag::U16 || tag == ir::TypeTag::U32 ||
         tag == ir::TypeTag::U64 || tag == ir::TypeTag::Usize ||
         tag == ir::TypeTag::Char || tag == ir::TypeTag::Rune;
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

// A register index rides on a memory operand when there is one; a
// constant index is folded into the displacement by the caller.
std::optional<RegRef> regref_of(const std::optional<Operand>& operand) {
  if (!operand.has_value()) {
    return std::nullopt;
  }
  if (const VReg* vreg = std::get_if<VReg>(&*operand); vreg != nullptr) {
    return RegRef{*vreg};
  }
  return std::nullopt;
}

class Selector {
 public:
  explicit Selector(const SelectContext& context)
      : storage_(context.storage),
        strings_(context.strings),
        spans_(context.spans),
        function_(context.function),
        bag_(context.bag),
        width_(context.width),
        rodata_(context.rodata),
        checks_(context.overlay),
        current_instr_idx_(ir::InstructionIdx::invalid()),
        site_names_(context.site_names),
        backtrace_(context.backtrace) {}

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
  [[nodiscard]] bool emit(const ir::Instruction& instr,
                          const diag::Span span,
                          const overlay::Entry* check);

  // The opcode groups `emit` dispatches to. An optional answer says
  // whether the group handled the opcode; no group means a scalar
  // instruction, whose lowering reads and writes one value.
  [[nodiscard]] std::optional<bool> emit_values(const ir::Instruction& instr,
                                                diag::Span span);
  [[nodiscard]] std::optional<bool> emit_control(const ir::Instruction& instr,
                                                 diag::Span span);
  [[nodiscard]] std::optional<bool> emit_memory(const ir::Instruction& instr,
                                                diag::Span span);
  [[nodiscard]] bool emit_arithmetic(const ir::Instruction& instr,
                                     diag::Span span,
                                     const overlay::Entry* check);
  [[nodiscard]] bool emit_cast(const ir::Instruction& instr, diag::Span span);

  // The runtime panic call a failed check ends the process through.
  void emit_panic_call(std::string_view message, str::StringPoolId site);

  // A check that guards the operands rather than the arithmetic result:
  // a shift count past the type's width, or a signed MIN / -1.
  [[nodiscard]] bool emit_check_prelude(const ir::Instruction& instr,
                                        const diag::Span span,
                                        const overlay::Entry& check);

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
  const overlay::Overlay* checks_ = nullptr;
  ir::Opcode current_op_ = ir::Opcode::Noop;

  Body body_;
  // The names body_.symbols holds, by index: a body that calls in a
  // loop names the same symbols once per call, and a linear scan per
  // name was quadratic in the number of calls.
  std::unordered_map<std::string, u32> symbol_ids_;
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
  // The instruction a backtrace would name, and the build's site table.
  ir::InstructionIdx current_instr_idx_;
  std::span<const str::StringPoolId> site_names_;
  bool backtrace_ = false;
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
  const auto found = symbol_ids_.find(symbol);
  if (found != symbol_ids_.end()) {
    return SymbolRef{found->second};
  }
  body_.symbols.push_back(symbol);
  symbol_ids_.emplace(symbol, static_cast<u32>(body_.symbols.size() - 1));
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
      return Operand{string_ref(
          rodata_.name(rodata_.intern(strings_.get(imm.data.str_id_value))))};
    }
    if (is_float(tag)) {
      // The IEEE bits of a float literal are the bytes the read-only
      // section holds; the allocator loads them through a GPR borrow.
      const u64 bits = imm.as_u64_bits(tag);
      const u32 bytes = tag == ir::TypeTag::F32 ? 4 : 8;
      const std::string data(reinterpret_cast<const char*>(&bits), bytes);
      return Operand{string_ref(rodata_.name(rodata_.intern(data)))};
    }
  }
  if (op.is<ir::FunctionIdx>()) {
    return Operand{string_ref(internal_symbol(op.as_function()))};
  }
  if (op.is<ir::ExternalFunctionIdx>()) {
    const auto& fn = storage_.external_functions()[op.as_external_function()];
    return Operand{string_ref(std::string(strings_.get(fn.meta.name)))};
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
  // Name the instruction and the operand shape, so a refusal points at
  // a construct rather than at an anonymous operand.
  const char* kind = "other";
  if (op.is<ir::ImmutableIdx>()) {
    kind = "imm";
  } else if (op.is<ir::RegisterIdx>()) {
    kind = "reg";
  } else if (op.is<ir::FunctionIdx>()) {
    kind = "function value";
  } else if (op.is<ir::ExternalFunctionIdx>()) {
    kind = "external function";
  }
  std::string what =
      std::string(ir::opcode_to_str(current_op_)) + " with " + kind;
  (void)unsupported(span, what);
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

  // A pointer's pointee can be named in a block the emitter reaches
  // later than the one that projects it (block order is not control
  // flow order). Gather every alloca's hint up front so a projection
  // in an earlier block still knows what its base points at.
  usize ir_instructions = 0;
  for (const ir::BlockIdx block_idx : function_.blocks) {
    ir_instructions += storage_.blocks()[block_idx].instrs.size();
    for (const ir::InstructionIdx instr_idx :
         storage_.blocks()[block_idx].instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      if (instr.op == ir::Opcode::Alloca && instr.dst.is_valid()) {
        note_pointee(instr.dst, storage_.registers()[instr.dst].type);
      }
    }
  }
  body_.insts.reserve(ir_instructions * 4 + 8);

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
      current_instr_idx_ = instr_idx;
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      const overlay::Entry* check = nullptr;
      if (checks_ != nullptr && instr_idx.idx < checks_->entries.size() &&
          checks_->entries[instr_idx.idx].kind != overlay::Check::None) {
        check = &checks_->entries[instr_idx.idx];
      }
      if (!emit(instr, span_of(instr_idx), check)) {
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
  u32 next_xmm = 0;
  u32 stack_offset = 0;
  // An argument that did not fit a register arrives on the stack, just
  // past the return address the call pushed.
  const auto from_stack = [&]() {
    const Operand slot =
        Operand{Mem{.base = Reg::Rbp,
                    .index = std::nullopt,
                    .scale = 1,
                    .disp = static_cast<i32>(16 + stack_offset)}};
    stack_offset += 8;
    return slot;
  };
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
        const ir::TypeTag tag = storage_.types()[param.type.idx].tag;
        if (is_float(tag)) {
          const MOp load = tag == ir::TypeTag::F32 ? MOp::Movss : MOp::Movsd;
          if (next_xmm >= ARG_XMM_COUNT) {
            push(make_inst(load, width_of(tag), Operand{VReg{vreg}},
                           from_stack()));
          } else {
            push(make_inst(load, width_of(tag), Operand{VReg{vreg}},
                           Operand{ARG_XMM_REGS[next_xmm++]}));
          }
        } else if (next_register >= ARG_REG_COUNT) {
          push(make_inst(MOp::Mov, width_of(tag), Operand{VReg{vreg}},
                         from_stack()));
        } else {
          push(make_inst(MOp::Mov, width_of(tag), Operand{VReg{vreg}},
                         Operand{ARG_REGS[next_register++]}));
        }
        break;
      }
      case Passage::TwoWord: {
        if (next_register + 1 >= ARG_REG_COUNT) {
          // The whole pair arrives on the stack: one wide move fills the
          // destination slot from the two words there. Advance the cursor
          // for both words, or a later stack argument reads the first
          // word of this pair again.
          const Operand low = from_stack();
          (void)from_stack();
          push(make_inst(MOp::Mov, 8, Operand{VReg{vreg}}, low));
          break;
        }
        push(make_inst(MOp::Mov, 8, Operand{SlotRef{vreg, 0}},
                       Operand{ARG_REGS[next_register++]}));
        push(make_inst(MOp::Mov, 8, Operand{SlotRef{vreg, 8}},
                       Operand{ARG_REGS[next_register++]}));
        break;
      }
      case Passage::ByAddress: {
        if (next_register >= ARG_REG_COUNT) {
          // The stack holds the address itself; borrow rax so the copy
          // below can use rdi, rsi and rcx in peace.
          push(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, from_stack()));
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

void Selector::emit_panic_call(const std::string_view message,
                               const str::StringPoolId site) {
  const SymbolRef text = string_ref(rodata_.name(rodata_.intern(message)));
  push(make_inst(MOp::Mov, 8, Operand{Reg::Rdi}, Operand{text}));
  push(make_inst(MOp::Mov, 8, Operand{Reg::Rsi},
                 Operand{Imm{static_cast<i64>(message.size())}}));
  if (site == str::EMPTY_STRING_ID) {
    push(make_inst(MOp::Xor, 8, Operand{Reg::Rdx}, Operand{Reg::Rdx}));
    push(make_inst(MOp::Xor, 8, Operand{Reg::Rcx}, Operand{Reg::Rcx}));
  } else {
    const std::string_view where = strings_.get(site);
    const SymbolRef name = string_ref(rodata_.name(rodata_.intern(where)));
    push(make_inst(MOp::Mov, 8, Operand{Reg::Rdx}, Operand{name}));
    push(make_inst(MOp::Mov, 8, Operand{Reg::Rcx},
                   Operand{Imm{static_cast<i64>(where.size())}}));
  }
  push(make_inst(MOp::Call, 8, Operand{SymbolRef{string_ref("alcy_panic")}}));
}

bool Selector::emit_check_prelude(const ir::Instruction& instr,
                                  const diag::Span span,
                                  const overlay::Entry& check) {
  if (check.kind == overlay::Check::ShiftRange) {
    const std::optional<Operand> raw =
        scalar_of(storage_.operands()[check.operand], span);
    if (!raw.has_value()) {
      return false;
    }
    const ir::TypeTag tag =
        storage_.types()[storage_.operands()[instr.operands.head()].type].tag;
    const u8 width = compute_width_of(tag);
    const u64 bits = tag == ir::TypeTag::I8 || tag == ir::TypeTag::U8 ||
                             tag == ir::TypeTag::Char
                         ? 8
                     : tag == ir::TypeTag::I16 || tag == ir::TypeTag::U16 ? 16
                     : tag == ir::TypeTag::I32 || tag == ir::TypeTag::U32 ||
                             tag == ir::TypeTag::Rune
                         ? 32
                         : 64;
    const u32 ok = fresh_label();
    push(make_inst(MOp::Mov, width, Operand{Reg::R9}, *raw));
    if (bits <= 0x7fffffffULL) {
      push(make_inst(MOp::Cmp, width, Operand{Reg::R9},
                     Operand{Imm{static_cast<i64>(bits)}}));
    } else {
      push(make_inst(MOp::Mov, width, Operand{Reg::R10},
                     Operand{Imm{static_cast<i64>(bits)}}));
      push(make_inst(MOp::Cmp, width, Operand{Reg::R9}, Operand{Reg::R10}));
    }
    push(make_jcc(Cond::B, Operand{Label{ok}}));
    emit_panic_call("shift out of range", check.site);
    MInst join = make_inst(MOp::Nop, 8);
    join.label = ok;
    push(join);
  } else if (check.kind == overlay::Check::MinOverOne) {
    const std::optional<Operand> dividend =
        scalar_of(storage_.operands()[instr.operands.head()], span);
    const std::optional<Operand> divisor =
        scalar_of(storage_.operands()[check.operand], span);
    if (!dividend.has_value() || !divisor.has_value()) {
      return false;
    }
    const ir::TypeTag tag =
        storage_.types()[storage_.operands()[instr.operands.head()].type].tag;
    const u8 width = compute_width_of(tag);
    // The dividend is sign-extended to the compute width, so the
    // sentinel to match is the source type's own minimum, not the
    // compute width's.
    const i64 minimum = tag == ir::TypeTag::I8    ? -128
                        : tag == ir::TypeTag::I16 ? -32768
                        : width == 4              ? INT32_MIN
                                                  : INT64_MIN;
    const u32 ok = fresh_label();
    push(make_inst(MOp::Mov, width, Operand{Reg::R9}, *dividend));
    if (minimum >= INT32_MIN && minimum <= INT32_MAX) {
      push(make_inst(MOp::Cmp, width, Operand{Reg::R9}, Operand{Imm{minimum}}));
    } else {
      push(
          make_inst(MOp::Mov, width, Operand{Reg::R10}, Operand{Imm{minimum}}));
      push(make_inst(MOp::Cmp, width, Operand{Reg::R9}, Operand{Reg::R10}));
    }
    push(make_jcc(Cond::Ne, Operand{Label{ok}}));
    push(make_inst(MOp::Mov, width, Operand{Reg::R9}, *divisor));
    push(make_inst(MOp::Cmp, width, Operand{Reg::R9}, Operand{Imm{-1}}));
    push(make_jcc(Cond::Ne, Operand{Label{ok}}));
    emit_panic_call("integer overflow", check.site);
    MInst join = make_inst(MOp::Nop, 8);
    join.label = ok;
    push(join);
  }

  return true;
}

bool Selector::emit(const ir::Instruction& instr,
                    const diag::Span span,
                    const overlay::Entry* check) {
  current_op_ = instr.op;
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;

  if (check != nullptr && check->kind != overlay::Check::None &&
      check->kind != overlay::Check::Overflow &&
      !emit_check_prelude(instr, span, *check)) {
    return false;
  }

  if (std::optional<bool> done = emit_values(instr, span); done.has_value()) {
    return *done;
  }
  if (std::optional<bool> done = emit_control(instr, span); done.has_value()) {
    return *done;
  }
  if (std::optional<bool> done = emit_memory(instr, span); done.has_value()) {
    return *done;
  }
  switch (instr.op) {
    case O::EnumTagLoad:
    case O::EnumTagSet:
    case O::EnumFieldAddr: return emit_enum(instr, ops, span);
    case O::Call: return emit_call(instr, ops, span);
    case O::AtomicLoad:
    case O::AtomicStore:
    case O::AtomicRmw:
    case O::AtomicCompareExchange:
    case O::Fence: (void)unsupported(span, "an atomic operation"); return false;
    default: break;
  }
  return emit_arithmetic(instr, span, check);
}

std::optional<bool> Selector::emit_values(const ir::Instruction& instr,
                                          const diag::Span span) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const usize count = ops.size();
  const bool has_dst = instr.dst.is_valid();

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
      const std::optional<Operand> src = operand_of(operand_at(instr, 0));
      if (!src.has_value()) {
        (void)unsupported(span, "a move of a value the backend cannot spell");
        return false;
      }
      push(make_inst(MOp::Mov, 8, dst_reg_operand(instr), *src));
      return true;
    }
    default: return std::nullopt;
  }
}

std::optional<bool> Selector::emit_control(const ir::Instruction& instr,
                                           const diag::Span span) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const usize count = ops.size();
  const bool has_dst = instr.dst.is_valid();

  switch (instr.op) {
    case O::Ret: {
      // An explicit `()` comes back as an operand of a type that holds
      // no bytes, and the function still returns void.
      if (!ops.empty()) {
        const ir::Operand& value = operand_at(instr, 0);
        const ir::TypeTag tag = storage_.types()[value.type].tag;
        if (tag != ir::TypeTag::Void && tag != ir::TypeTag::Never) {
          const ir::TypeLayout layout = storage_.layout_of(value.type, width_);
          const Passage passage = passage_of(value.type);
          if (is_float(tag)) {
            const std::optional<Operand> src = scalar_of(value, span);
            if (!src.has_value()) {
              return false;
            }
            push(make_inst(tag == ir::TypeTag::F32 ? MOp::Movss : MOp::Movsd,
                           width_of(tag), Operand{RETURN_XMM_REG}, *src));
          } else if (passage == Passage::ByAddress) {
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
          } else if (passage == Passage::TwoWord &&
                     value.is<ir::ImmutableIdx>()) {
            // A string literal is a constant pair: the data section
            // holds its bytes and the length rides beside the pointer.
            const ir::Immutable& imm =
                storage_.immutables()[value.as_immutable()];
            if (storage_.types()[imm.type.idx].tag != ir::TypeTag::Str) {
              (void)unsupported(span,
                                "a constant return the backend cannot "
                                "spell");
              return false;
            }
            const std::string_view bytes = strings_.get(imm.data.str_id_value);
            const SymbolRef text =
                string_ref(rodata_.name(rodata_.intern(bytes)));
            push(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{text}));
            push(make_inst(MOp::Mov, 8, Operand{Reg::Rdx},
                           Operand{Imm{static_cast<i64>(bytes.size())}}));
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
      const ir::BlockIdx target = operand_at(instr, 0).as_block();
      const ir::Block& target_block = storage_.blocks()[target];
      DCHECK(count == 1 + target_block.block_params.size());
      for (u32 param = 0; param + 1 < count; ++param) {
        const ir::BlockParam& block_param =
            storage_.block_params()[target_block.block_params[param]];
        const std::optional<Operand> value =
            scalar_of(operand_at(instr, param + 1), span);
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
      const ir::BlockIdx taken = operand_at(instr, 1).as_block();
      const ir::BlockIdx other = operand_at(instr, 2).as_block();
      if (operand_at(instr, 0).is<ir::ImmutableIdx>()) {
        // A constant condition is the branch it names.
        const ir::Immutable& imm =
            storage_.immutables()[operand_at(instr, 0).as_immutable()];
        const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
        const bool truth = is_integer(tag) && imm.as_u64_integer(tag) != 0;
        push(make_inst(
            MOp::Jmp, 8,
            Operand{Label{body_.block_labels[(truth ? taken : other).idx]}}));
        return true;
      }
      const std::optional<Operand> cond = scalar_of(operand_at(instr, 0), span);
      if (!cond.has_value()) {
        return false;
      }
      push(make_inst(
          MOp::Cmp,
          compute_width_of(storage_.types()[operand_at(instr, 0).type].tag),
          *cond, Operand{Imm{0}}));
      push(make_jcc(Cond::Ne, Operand{Label{body_.block_labels[taken.idx]}}));
      push(make_inst(MOp::Jmp, 8,
                     Operand{Label{body_.block_labels[other.idx]}}));
      return true;
    }
    case O::Switch: {
      // operands = [value, default, (case_imm, case_block)...].
      DCHECK(count >= 2);
      const std::optional<Operand> value =
          scalar_of(operand_at(instr, 0), span);
      if (!value.has_value()) {
        return false;
      }
      const u8 width =
          compute_width_of(storage_.types()[operand_at(instr, 0).type].tag);
      for (u32 idx = 2; idx + 1 < count; idx += 2) {
        const std::optional<Operand> case_value =
            operand_of(operand_at(instr, idx));
        if (!case_value.has_value()) {
          (void)unsupported(span, "a case the backend cannot spell");
          return false;
        }
        Operand right = *case_value;
        if (width == 8) {
          if (const Imm* imm = std::get_if<Imm>(&right);
              imm != nullptr &&
              (imm->value > 0x7fffffffLL || imm->value < -0x80000000LL)) {
            push(make_inst(MOp::Mov, 8, Operand{Reg::Rdx}, right));
            right = Operand{Reg::Rdx};
          }
        }
        push(make_inst(MOp::Cmp, width, *value, right));
        push(make_jcc(
            Cond::E,
            Operand{Label{body_.block_labels
                              [operand_at(instr, idx + 1).as_block().idx]}}));
      }
      push(make_inst(
          MOp::Jmp, 8,
          Operand{
              Label{body_.block_labels[operand_at(instr, 1).as_block().idx]}}));
      return true;
    }
    case O::Select: {
      // operands = [cond, true_val, false_val].
      DCHECK(count == 3);
      if (!has_dst) {
        return true;
      }
      const std::optional<Operand> cond = scalar_of(operand_at(instr, 0), span);
      const std::optional<Operand> yes = scalar_of(operand_at(instr, 1), span);
      const std::optional<Operand> no = scalar_of(operand_at(instr, 2), span);
      if (!cond.has_value() || !yes.has_value() || !no.has_value()) {
        return false;
      }
      const u32 taken = fresh_label();
      const u32 done = fresh_label();
      push(make_inst(
          MOp::Cmp,
          compute_width_of(storage_.types()[operand_at(instr, 0).type].tag),
          *cond, Operand{Imm{0}}));
      push(make_jcc(Cond::Ne, Operand{Label{taken}}));
      push(make_inst(MOp::Mov, 8, dst_reg_operand(instr), *no));
      push(make_inst(MOp::Jmp, 8, Operand{Label{done}}));
      MInst yes_move = make_inst(MOp::Mov, 8, dst_reg_operand(instr), *yes);
      yes_move.label = taken;
      push(yes_move);
      // The join, which carries no code of its own.
      MInst join = make_inst(MOp::Nop, 8);
      join.label = done;
      push(join);
      return true;
    }
    default: return std::nullopt;
  }
}

std::optional<bool> Selector::emit_memory(const ir::Instruction& instr,
                                          const diag::Span span) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const usize count = ops.size();
  const bool has_dst = instr.dst.is_valid();

  switch (instr.op) {
    case O::TypeSizeOf:
    case O::TypeAlignOf: {
      if (!has_dst) {
        return true;
      }
      const ir::TypeLayout layout = storage_.layout_of(instr.measure, width_);
      push(make_inst(
          MOp::Mov, 8, dst_reg_operand(instr),
          Operand{Imm{static_cast<i64>(
              instr.op == O::TypeSizeOf ? layout.size : layout.align)}}));
      return true;
    }
    case O::Alloca: {
      if (!has_dst) {
        return true;
      }
      // The element count is a constant when present: this IR never
      // carries a slot whose size is a value.
      u64 elements = 1;
      if (!ops.empty()) {
        if (!operand_at(instr, 0).is<ir::ImmutableIdx>()) {
          (void)unsupported(span, "a slot whose size is a value");
          return false;
        }
        const ir::Immutable& count_imm =
            storage_.immutables()[operand_at(instr, 0).as_immutable()];
        elements =
            count_imm.as_u64_integer(storage_.types()[count_imm.type.idx].tag);
      }
      const ir::TypeLayout layout = storage_.layout_of(dst_type(instr), width_);
      body_.allocas.push_back(Body::Alloca{
          .vreg = vreg_of(instr.dst, dst_type(instr)),
          .size = layout.size * elements,
          .align = layout.align,
      });
      note_alloca(instr.dst);
      note_pointee(instr.dst, dst_type(instr));
      return true;
    }
    case O::Load: {
      DCHECK(count == 1);
      if (!has_dst) {
        return true;
      }
      if (!operand_at(instr, 0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::TypeTag loaded = dst_value_tag(instr);
      // A value wider than one register crosses as its whole slot. The
      // allocator turns this into the byte copy the destination needs;
      // spelling the halves as separate moves would ask the encoder for
      // a memory-to-memory move x86 does not have.
      const ir::TypeLayout layout = storage_.layout_of(dst_type(instr), width_);
      if (passage_of(dst_type(instr)) != Passage::Register ||
          is_aggregate(loaded)) {
        const u32 vreg = vreg_of(instr.dst, dst_type(instr));
        push(make_inst(
            MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
            Operand{VReg{vreg}},
            Operand{Mem{.base = VReg{vreg_of(operand_at(instr, 0).as_register(),
                                             operand_at(instr, 0).type)},
                        .index = std::nullopt,
                        .scale = 1,
                        .disp = 0}}));
        return true;
      }
      push(make_inst(
          MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
          dst_reg_operand(instr),
          Operand{Mem{.base = VReg{vreg_of(operand_at(instr, 0).as_register(),
                                           operand_at(instr, 0).type)},
                      .index = std::nullopt,
                      .scale = 1,
                      .disp = 0}}));
      return true;
    }
    case O::Store: {
      // operands = [value, ptr].
      DCHECK(count == 2);
      if (!operand_at(instr, 1).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::Operand& value = operand_at(instr, 0);
      const ir::TypeTag tag = storage_.types()[value.type].tag;
      const ir::TypeLayout layout = storage_.layout_of(value.type, width_);
      if (is_aggregate(tag) || passage_of(value.type) != Passage::Register) {
        const u32 dst = vreg_of(operand_at(instr, 1).as_register(),
                                operand_at(instr, 1).type);
        if (!value.is<ir::RegisterIdx>()) {
          if (tag == ir::TypeTag::Str && value.is<ir::ImmutableIdx>()) {
            const ir::Immutable& imm =
                storage_.immutables()[value.as_immutable()];
            const std::string_view text = strings_.get(imm.data.str_id_value);
            const SymbolRef bytes =
                string_ref(rodata_.name(rodata_.intern(text)));
            push(make_inst(MOp::Mov, 8,
                           Operand{Mem{.base = VReg{dst},
                                       .index = std::nullopt,
                                       .scale = 1,
                                       .disp = 0}},
                           Operand{bytes}));
            push(make_inst(MOp::Mov, 8,
                           Operand{Mem{.base = VReg{dst},
                                       .index = std::nullopt,
                                       .scale = 1,
                                       .disp = 8}},
                           Operand{Imm{static_cast<i64>(text.size())}}));
            return true;
          }
          (void)unsupported(span, "a constant aggregate store");
          return false;
        }
        // An aggregate crosses as the bytes its slot holds, which is
        // the whole of what the store has to say. A narrow aggregate
        // must not write a whole word past its slot.
        push(make_inst(
            MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
            Operand{Mem{.base = VReg{dst},
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
      const Operand dst =
          Operand{Mem{.base = VReg{vreg_of(operand_at(instr, 1).as_register(),
                                           operand_at(instr, 1).type)},
                      .index = std::nullopt,
                      .scale = 1,
                      .disp = 0}};
      if (is_float(tag)) {
        push(make_inst(tag == ir::TypeTag::F32 ? MOp::Movss : MOp::Movsd,
                       width_of(tag), dst, *src));
        return true;
      }
      push(make_inst(MOp::Mov,
                     static_cast<u8>(layout.size > 8 ? 8 : layout.size), dst,
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
      if (!operand_at(instr, 0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::RegisterIdx base = operand_at(instr, 0).as_register();
      std::optional<ir::TypeIdx> ty = pointee_hint(base);
      if (!ty.has_value()) {
        (void)unsupported(span, "a projection of an untracked pointer");
        return false;
      }
      i64 offset = 0;
      std::optional<Operand> index;
      u64 scale = 1;
      for (u32 idx = 1; idx < count; ++idx) {
        const std::optional<Operand> step = operand_of(operand_at(instr, idx));
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
          } else if (element.size != 0) {
            index = *step;
            scale = element.size;
          }
          // A zero-sized element has no stride: every index names the
          // same address, so no scaled index is encoded at all.
          ty = shape.element;
          continue;
        }
        if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Tuple) {
          const Imm* imm = std::get_if<Imm>(&*step);
          if (imm == nullptr) {
            (void)unsupported(span, "a dynamic field projection");
            return false;
          }
          const bool is_struct = tag == ir::TypeTag::Struct;
          const ir::StructType* shape =
              is_struct
                  ? &storage_
                         .struct_types()[storage_.types()[ty->idx].as_struct()]
                  : nullptr;
          const ir::TypeIdxRange fields =
              is_struct
                  ? shape->fields
                  : storage_.tuple_types()[storage_.types()[ty->idx].as_tuple()]
                        .elements;
          const auto hints =
              shape == nullptr
                  ? std::pair<const ir::TypeLayoutHints*, u32>{nullptr, 0}
                  : ir::field_hints_of(storage_.state(), *shape);
          offset += static_cast<i64>(ir::field_offset(
              storage_.state(), fields, static_cast<u32>(imm->value), width_,
              hints.first, hints.second));
          ty = fields[static_cast<u32>(imm->value)];
          continue;
        }
        // A two-word value is built one word at a time: `str_from_parts`
        // projects a `str` at indices 0 and 1, each an eight-byte word.
        // Other scalars are indexed by their own layout size.
        if (passage_of(*ty) == Passage::TwoWord) {
          if (const Imm* imm = std::get_if<Imm>(&*step); imm != nullptr) {
            offset += imm->value * 8;
          } else {
            index = *step;
            scale = 8;
          }
          continue;
        }
        const ir::TypeLayout scalar = storage_.layout_of(*ty, width_);
        if (const Imm* imm = std::get_if<Imm>(&*step); imm != nullptr) {
          offset += static_cast<i64>(scalar.size) * imm->value;
        } else if (scalar.size != 0) {
          index = *step;
          scale = scalar.size;
        }
      }
      // A projection's destination names the element it reaches, and
      // the register holds its address, so the vreg is a pointer's
      // width whatever the element is.
      const u32 address =
          vreg_of(instr.dst, ir::primitive_idx(ir::TypeTag::Ptr));
      push(make_inst(
          MOp::Lea, 8, Operand{VReg{address}},
          Operand{Mem{.base = VReg{vreg_of(base, operand_at(instr, 0).type)},
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
      if (!operand_at(instr, 0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      const ir::TypeTag pointee_tag = dst_value_tag(instr);
      const bool pointer = pointee_tag == ir::TypeTag::Ref ||
                           pointee_tag == ir::TypeTag::MutRef ||
                           pointee_tag == ir::TypeTag::Ptr ||
                           pointee_tag == ir::TypeTag::RawPtr ||
                           pointee_tag == ir::TypeTag::RawMutPtr;
      if (!pointer) {
        (void)unsupported(span, "an element projection of a non-reference");
        return false;
      }
      const ir::TypeIdx element =
          storage_.ref_types()[storage_.types()[dst_type(instr).idx].as_ref()]
              .pointee;
      const ir::TypeLayout layout = storage_.layout_of(element, width_);
      const std::optional<Operand> index =
          scalar_of(operand_at(instr, 1), span);
      if (!index.has_value()) {
        return false;
      }
      // A constant element index folds into the displacement; a register
      // one rides through the scaled-index field.
      i64 disp = 0;
      std::optional<RegRef> scaled;
      if (const Imm* imm = std::get_if<Imm>(&*index); imm != nullptr) {
        disp = imm->value * static_cast<i64>(layout.size);
      } else if (layout.size != 0) {
        scaled = regref_of(index);
        if (!scaled.has_value()) {
          (void)unsupported(span, "an element offset the backend cannot spell");
          return false;
        }
      }
      // A zero-sized element strides by zero; the address is the base.
      push(make_inst(
          MOp::Lea, 8, dst_reg_operand(instr),
          Operand{Mem{.base = VReg{vreg_of(operand_at(instr, 0).as_register(),
                                           operand_at(instr, 0).type)},
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
        const std::optional<Operand> side =
            scalar_of(operand_at(instr, which), span);
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
      // operands = [dst, value, len]. `rep stosb` reads the fill byte
      // from AL and the count from RCX. Loading a computed count into
      // RCX uses RAX as a scratch, which would clobber AL, so the fill
      // byte is installed last.
      DCHECK(count == 3);
      for (const u32 which : {0u, 2u, 1u}) {
        const std::optional<Operand> side =
            scalar_of(operand_at(instr, which), span);
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
      if (operand_at(instr, 0).is<ir::ImmutableIdx>()) {
        // A literal's fields are constants: the bytes live in the data
        // section and the length is the count of them.
        const ir::Immutable& imm =
            storage_.immutables()[operand_at(instr, 0).as_immutable()];
        const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
        if (tag != ir::TypeTag::Str) {
          (void)unsupported(span,
                            "a literal field the backend cannot "
                            "spell");
          return false;
        }
        if (!operand_at(instr, 1).is<ir::ImmutableIdx>()) {
          (void)unsupported(span, "a dynamic field projection");
          return false;
        }
        const ir::Immutable& index_imm =
            storage_.immutables()[operand_at(instr, 1).as_immutable()];
        const u64 which =
            index_imm.as_u64_integer(storage_.types()[index_imm.type.idx].tag);
        const SymbolRef symbol = string_ref(
            rodata_.name(rodata_.intern(strings_.get(imm.data.str_id_value))));
        if (which == 0) {
          push(make_inst(MOp::Mov, 8, dst_reg_operand(instr), Operand{symbol}));
        } else {
          push(make_inst(MOp::Mov, 8, dst_reg_operand(instr),
                         Operand{Imm{static_cast<i64>(
                             strings_.get(imm.data.str_id_value).size())}}));
        }
        return true;
      }
      // A value's field rides in its slot at the offset the layout
      // gives it, which the aggregate's own field walk publishes.
      if (!operand_at(instr, 0).is<ir::RegisterIdx>()) {
        (void)unsupported(span, "a memory operand the backend cannot spell");
        return false;
      }
      if (!operand_at(instr, 1).is<ir::ImmutableIdx>()) {
        (void)unsupported(span, "a dynamic field projection");
        return false;
      }
      const ir::RegisterIdx aggregate = operand_at(instr, 0).as_register();
      const ir::TypeIdx aggregate_type = operand_at(instr, 0).type;
      const ir::Immutable& index_imm =
          storage_.immutables()[operand_at(instr, 1).as_immutable()];
      const u64 which =
          index_imm.as_u64_integer(storage_.types()[index_imm.type.idx].tag);
      i64 offset = 0;
      const ir::TypeTag tag = storage_.types()[aggregate_type.idx].tag;
      if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Tuple) {
        const ir::TypeIdxRange fields =
            tag == ir::TypeTag::Struct
                ? storage_
                      .struct_types()[storage_.types()[aggregate_type.idx]
                                          .as_struct()]
                      .fields
                : storage_
                      .tuple_types()[storage_.types()[aggregate_type.idx]
                                         .as_tuple()]
                      .elements;
        const auto hints =
            tag == ir::TypeTag::Struct
                ? ir::field_hints_of(
                      storage_.state(),
                      storage_.struct_types()
                          [storage_.types()[aggregate_type.idx].as_struct()])
                : std::pair<const ir::TypeLayoutHints*, u32>{nullptr, 0};
        offset = static_cast<i64>(
            ir::field_offset(storage_.state(), fields, static_cast<u32>(which),
                             width_, hints.first, hints.second));
      } else if (tag == ir::TypeTag::Str ||
                 ((tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) &&
                  passage_of(aggregate_type) == Passage::TwoWord)) {
        // The pair a slice reference carries: the bytes, then the length.
        offset = static_cast<i64>(which * 8);
      } else {
        (void)unsupported(span,
                          "a field of an aggregate the backend has "
                          "no layout for yet");
        return false;
      }
      const ir::TypeLayout layout = storage_.layout_of(dst_type(instr), width_);
      // The pair rides in the register's own slot: reading a field is
      // the slot reference, not a dereference of the word it holds.
      push(make_inst(
          MOp::Mov, static_cast<u8>(layout.size > 8 ? 8 : layout.size),
          dst_reg_operand(instr),
          Operand{SlotRef{vreg_of(aggregate, operand_at(instr, 0).type),
                          static_cast<i32>(offset)}}));
      return true;
    }
    case O::InsertValue: {
      (void)unsupported(span, "an aggregate field write");
      return false;
    }
    default: return std::nullopt;
  }
}

bool Selector::emit_arithmetic(const ir::Instruction& instr,
                               const diag::Span span,
                               const overlay::Entry* check) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const usize count = ops.size();
  const auto at = [&](const u32 index) { return operand_at(instr, index); };
  // A checked operation that overflows ends the process through the
  // runtime panic; the carry flag serves unsigned arithmetic and the
  // overflow flag signed arithmetic.
  const auto check_overflow = [&](const Cond condition) {
    const u32 ok = fresh_label();
    const Cond no_overflow = condition == Cond::O   ? Cond::No
                             : condition == Cond::B ? Cond::Ae
                                                    : condition;
    push(make_jcc(no_overflow, Operand{Label{ok}}));
    emit_panic_call("integer overflow",
                    check != nullptr ? check->site : str::EMPTY_STRING_ID);
    MInst join = make_inst(MOp::Nop, 8);
    join.label = ok;
    push(join);
  };

  // Everything left reads one or two scalars and writes one.
  if (!defines_dst(instr)) {
    return true;
  }
  const u8 compute = compute_width_of(dst_value_tag(instr));
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
      const bool checked_narrow =
          check != nullptr && check->kind == overlay::Check::Overflow &&
          (instr.op == O::IntAdd || instr.op == O::IntSub) &&
          width_of(dst_value_tag(instr)) < 4;
      const u8 alu_width =
          checked_narrow ? width_of(dst_value_tag(instr)) : compute;
      push(make_inst(MOp::Mov, alu_width, dst_reg_operand(instr), *lhs));
      const std::optional<Operand> rhs = scalar_of(operand_at(instr, 1), span);
      if (!rhs.has_value()) {
        return false;
      }
      // The opcode carries a 32-bit immediate; a wider constant takes a
      // register the instruction is free to clobber.
      Operand right = *rhs;
      if (const Imm* value = std::get_if<Imm>(&right);
          value != nullptr &&
          (value->value > 0x7fffffffLL || value->value < -0x80000000LL)) {
        push(make_inst(MOp::Mov, alu_width, Operand{Reg::Rdx}, right));
        right = Operand{Reg::Rdx};
      }
      push(make_inst(alu_of(instr.op), alu_width, dst_reg_operand(instr),
                     right));
      if ((instr.op == O::IntAdd || instr.op == O::IntSub) &&
          check != nullptr && check->kind == overlay::Check::Overflow) {
        check_overflow(is_signed(dst_value_tag(instr)) ? Cond::O : Cond::B);
      }
      return true;
    }
    case O::IntMul:
    case O::IntMulWrap: {
      DCHECK(count == 2);
      const std::optional<Operand> rhs = scalar_of(operand_at(instr, 1), span);
      if (!rhs.has_value()) {
        return false;
      }
      const bool checked = instr.op == O::IntMul && check != nullptr &&
                           check->kind == overlay::Check::Overflow;
      if (checked && !is_signed(dst_value_tag(instr))) {
        // `imul` reports *signed* overflow through its carry flag, which
        // a product that fits unsigned can still trip. The one-operand
        // `mul` sets carry from the high half of an unsigned product.
        // Resolve the right factor into rcx *before* the left into rax:
        // both may be vregs and allocation borrows rax as its first
        // scratch. Reading rax first would let the second resolution
        // overwrite the left factor.
        push(make_inst(MOp::Mov, compute, Operand{Reg::Rcx}, *rhs));
        push(make_inst(MOp::Mov, compute, Operand{Reg::Rax}, *lhs));
        push(make_inst(MOp::Umul, compute, Operand{Reg::Rcx}));
        push(make_inst(MOp::Mov, compute, dst_reg_operand(instr),
                       Operand{Reg::Rax}));
        check_overflow(Cond::B);
        return true;
      }
      push(make_inst(MOp::Mov, compute, dst_reg_operand(instr), *lhs));
      if (const Imm* immediate = std::get_if<Imm>(&*rhs);
          immediate != nullptr) {
        // The two-operand form reads a register, so a constant takes one
        // the instruction is free to clobber.
        push(make_inst(MOp::Mov, compute, Operand{Reg::Rcx}, *rhs));
        push(make_inst(MOp::Imul, compute, dst_reg_operand(instr),
                       Operand{Reg::Rcx}));
      } else {
        push(make_inst(MOp::Imul, compute, dst_reg_operand(instr), *rhs));
      }
      if (checked) {
        check_overflow(Cond::O);
      }
      return true;
    }
    case O::FAdd:
    case O::FSub:
    case O::FMul:
    case O::FDiv: {
      DCHECK(count == 2);
      const u8 w = width_of(dst_value_tag(instr));
      const MOp op = instr.op == O::FAdd   ? MOp::Fadd
                     : instr.op == O::FSub ? MOp::Fsub
                     : instr.op == O::FMul ? MOp::Fmul
                                           : MOp::Fdiv;
      const std::optional<Operand> lhs = scalar_of(operand_at(instr, 0), span);
      const std::optional<Operand> rhs = scalar_of(operand_at(instr, 1), span);
      if (!lhs.has_value() || !rhs.has_value()) {
        return false;
      }
      push(make_inst(op, w, dst_reg_operand(instr), *lhs, *rhs));
      return true;
    }
    case O::FNeg: {
      DCHECK(count == 1);
      const u8 w = width_of(dst_value_tag(instr));
      const u64 sign = w == 4 ? 0x80000000ULL : 0x8000000000000000ULL;
      const std::string mask(reinterpret_cast<const char*>(&sign), w);
      const SymbolRef mask_ref = string_ref(rodata_.name(rodata_.intern(mask)));
      const std::optional<Operand> src = scalar_of(operand_at(instr, 0), span);
      if (!src.has_value()) {
        return false;
      }
      push(make_inst(MOp::Fneg, w, dst_reg_operand(instr), *src,
                     Operand{mask_ref}));
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
      if (is_signed(dst_value_tag(instr))) {
        push(make_inst(MOp::Cqo, compute));
      } else {
        push(
            make_inst(MOp::Xor, compute, Operand{Reg::Rdx}, Operand{Reg::Rdx}));
      }
      const std::optional<Operand> rhs = scalar_of(operand_at(instr, 1), span);
      if (!rhs.has_value()) {
        return false;
      }
      push(make_inst(instr.op == O::IntDiv || instr.op == O::IntRem ? MOp::Idiv
                                                                    : MOp::Udiv,
                     compute, *rhs));
      push(make_inst(MOp::Mov, compute, dst_reg_operand(instr),
                     instr.op == O::IntDiv || instr.op == O::UintDiv
                         ? Operand{Reg::Rax}
                         : Operand{Reg::Rdx}));
      return true;
    }
    case O::ShiftLeft:
    case O::ArithmeticShiftRight:
    case O::LogicalShiftRight: {
      DCHECK(count == 2);
      push(make_inst(MOp::Mov, compute, dst_reg_operand(instr), *lhs));
      const std::optional<Operand> rhs = scalar_of(operand_at(instr, 1), span);
      if (!rhs.has_value()) {
        return false;
      }
      MOp op = shift_mop_of(instr.op);
      // A count in a register has to reach `cl`, which is the only
      // register a shift reads it from; a constant rides in the
      // instruction.
      if (const Imm* imm = std::get_if<Imm>(&*rhs); imm != nullptr) {
        push(make_inst(op, compute, dst_reg_operand(instr), *rhs));
      } else {
        push(make_inst(MOp::Mov, 1, Operand{Reg::Rcx}, *rhs));
        push(make_inst(op, compute, dst_reg_operand(instr), Operand{Reg::Rcx}));
      }
      return true;
    }
    case O::Not: {
      DCHECK(count == 1);
      push(make_inst(MOp::Mov, compute, dst_reg_operand(instr), *lhs));
      if (dst_value_tag(instr) == ir::TypeTag::I1) {
        // A bool's negation is logical, and a bool rides as zero or
        // one, so flipping the one bit is the whole answer.
        push(make_inst(MOp::Xor, compute, dst_reg_operand(instr),
                       Operand{Imm{1}}));
      } else {
        push(make_inst(MOp::Not, compute, dst_reg_operand(instr)));
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
      const std::optional<Operand> rhs = scalar_of(operand_at(instr, 1), span);
      if (!rhs.has_value()) {
        return false;
      }
      // A floating compare sets ZF, PF (unordered) and CF. The primary
      // condition is then combined with the unordered flag: equality and
      // the ordered relations require an ordered pair, while `!=` holds
      // for a NaN.
      if (is_float(storage_.types()[operand_at(instr, 0).type].tag)) {
        const u8 w = width_of(storage_.types()[operand_at(instr, 0).type].tag);
        push(make_inst(MOp::Fcom, w, *lhs, *rhs));
        const Cond primary = instr.op == O::Eq   ? Cond::E
                             : instr.op == O::Ne ? Cond::Ne
                             : instr.op == O::Lt ? Cond::B
                             : instr.op == O::Le ? Cond::Be
                             : instr.op == O::Gt ? Cond::A
                                                 : Cond::Ae;
        push(make_setcc(primary, Operand{Reg::Rcx}));
        const Cond unordered = instr.op == O::Ne ? Cond::P : Cond::Np;
        push(make_setcc(unordered, Operand{Reg::Rdx}));
        push(make_inst(instr.op == O::Ne ? MOp::Or : MOp::And, 1,
                       Operand{Reg::Rcx}, Operand{Reg::Rdx}));
        push(make_inst(MOp::Mov, 1, dst_reg_operand(instr), Operand{Reg::Rcx}));
        return true;
      }
      // The compare runs at its operands' width, which is the width the
      // value holds and not the bool it answers.
      const u8 compare =
          compute_width_of(storage_.types()[operand_at(instr, 0).type].tag);
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
          compare_of(
              instr.op,
              is_signed(storage_.types()[operand_at(instr, 0).type].tag)),
          dst_reg_operand(instr)));
      return true;
    }
    case O::TypeCast: return emit_cast(instr, span);
    default: {
      (void)unsupported(span,
                        "an instruction the backend has no lowering "
                        "for");
      return false;
    }
  }
}

bool Selector::emit_cast(const ir::Instruction& instr, const diag::Span span) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const usize count = ops.size();
  const auto at = [&](const u32 index) { return operand_at(instr, index); };
  const std::optional<Operand> lhs = scalar_of(at(0), span);
  if (!lhs.has_value()) {
    return false;
  }

  switch (instr.op) {
    case O::TypeCast: {
      DCHECK(count == 1);
      const ir::TypeTag src = storage_.types()[operand_at(instr, 0).type].tag;
      const ir::TypeTag dst = dst_value_tag(instr);
      if (is_float(src) && is_float(dst)) {
        if (src == dst) {
          push(make_inst(src == ir::TypeTag::F32 ? MOp::Movss : MOp::Movsd,
                         width_of(src), dst_reg_operand(instr), *lhs));
        } else {
          push(make_inst(MOp::CvtF2F, width_of(src), dst_reg_operand(instr),
                         *lhs));
        }
        return true;
      }
      if (is_integer(src) && is_float(dst)) {
        // The float type picks ss/sd; the integer type picks REX.W.
        // They are different widths on a mixed conversion, so carry
        // them separately as the opcode width and a third operand.
        const i64 tagged =
            is_signed(src) ? width_of(src) : -static_cast<i64>(width_of(src));
        push(make_inst(MOp::CvtI2F, width_of(dst), dst_reg_operand(instr), *lhs,
                       Operand{Imm{tagged}}));
        return true;
      }
      if (is_float(src) && is_integer(dst)) {
        const i64 bytes =
            dst == ir::TypeTag::I8 || dst == ir::TypeTag::U8 ||
                    dst == ir::TypeTag::Char
                ? 1
            : dst == ir::TypeTag::I16 || dst == ir::TypeTag::U16 ? 2
            : dst == ir::TypeTag::I32 || dst == ir::TypeTag::U32 ||
                    dst == ir::TypeTag::Rune
                ? 4
                : 8;
        const i64 tagged =
            dst == ir::TypeTag::I1 ? -16
            : (dst == ir::TypeTag::U8 || dst == ir::TypeTag::U16 ||
               dst == ir::TypeTag::U32 || dst == ir::TypeTag::U64 ||
               dst == ir::TypeTag::Char || dst == ir::TypeTag::Rune)
                ? -bytes
                : bytes;
        push(make_inst(MOp::CvtF2I, width_of(src), dst_reg_operand(instr), *lhs,
                       Operand{Imm{tagged}}));
        return true;
      }
      if (is_integer(src) && is_integer(dst)) {
        const u8 from = width_of(src);
        const u8 to = width_of(dst_value_tag(instr));
        if (from < to) {
          // A wider value reads the narrow one's own bytes and
          // re-derives the rest.
          push(make_inst(is_signed(src) ? MOp::Movsx : MOp::Movzx, from,
                         dst_reg_operand(instr), *lhs));
        } else {
          push(make_inst(MOp::Mov, 8, dst_reg_operand(instr), *lhs));
        }
        return true;
      }
      // A pointer and an integer of the machine's width are the same
      // bytes; two pointers are too.
      push(make_inst(MOp::Mov, 8, dst_reg_operand(instr), *lhs));
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
    if (ir::enum_is_zst(storage_.state(), enum_type)) {
      push(make_inst(MOp::Mov, 4, dst_reg_operand(instr), Operand{Imm{0}}));
      return true;
    }
    if (!niche.has) {
      const u8 tag_width = ir::enum_tag_width(storage_.state(), enum_type);
      if (tag_width < 4) {
        push(make_inst(MOp::Movzx, tag_width, dst_reg_operand(instr), place));
      } else {
        push(make_inst(MOp::Mov, tag_width, dst_reg_operand(instr), place));
      }
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
    if (ir::enum_is_zst(storage_.state(), enum_type)) {
      return true;
    }
    if (!niche.has) {
      const std::optional<Operand> tag = scalar_of(operand_at(instr, 1), span);
      if (!tag.has_value()) {
        return false;
      }
      push(make_inst(MOp::Mov, ir::enum_tag_width(storage_.state(), enum_type),
                     place, *tag));
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
  const u64 disc = ir::enum_tag_width(storage_.state(), enum_type);
  const u64 payload_start =
      area.align <= 1 ? disc
                      : (disc + area.align - 1) / area.align * area.align;
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
    symbol_id = string_ref(symbol).id;
  }

  // The argument registers, in the order the call fills them. A
  // wide answer took the first for the caller's slot.
  u32 next_register = 0;
  u32 next_xmm = 0;
  u32 stack_offset = 0;
  // An argument that did not fit a register is written into the frame's
  // outgoing area, just above the current stack pointer.
  const auto spill = [&](const Operand& value) {
    const Operand slot = Operand{Mem{.base = Reg::Rsp,
                                     .index = std::nullopt,
                                     .scale = 1,
                                     .disp = static_cast<i32>(stack_offset)}};
    push(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, value));
    push(make_inst(MOp::Mov, 8, slot, Operand{Reg::Rax}));
    stack_offset += 8;
    body_.outgoing = std::max(body_.outgoing, stack_offset);
  };
  if (defines_dst(instr) && passage_of(dst_type(instr)) == Passage::ByAddress) {
    // The answer lands in the slot the destination vreg holds, and
    // the callee writes it there; nothing reads `rax`.
    push(make_inst(MOp::Lea, 8, Operand{ARG_REGS[next_register++]},
                   Operand{SlotRef{vreg_of(instr.dst, dst_type(instr)), 0}}));
  }
  if (indirect) {
    // A function value is the code and the environment it owns. The
    // environment is the first argument the closure body reads, before
    // any source argument.
    const u32 vreg =
        vreg_of(operand_at(instr, 0).as_register(), operand_at(instr, 0).type);
    push(make_inst(MOp::Mov, 8, Operand{ARG_REGS[next_register++]},
                   Operand{SlotRef{vreg, 8}}));
  }
  for (u32 arg = 1; arg < count; ++arg) {
    const ir::Operand& value = operand_at(instr, arg);
    const ir::TypeTag tag = storage_.types()[value.type].tag;
    if (is_float(tag)) {
      const MOp load = tag == ir::TypeTag::F32 ? MOp::Movss : MOp::Movsd;
      const std::optional<Operand> side = scalar_of(value, span);
      if (!side.has_value()) {
        return false;
      }
      if (next_xmm >= ARG_XMM_COUNT) {
        // SysV passes the ninth and later float arguments on the stack.
        // Store the scalar through a GPR, then hand the slot to the
        // callee; a float cannot be stored to memory directly here.
        const Operand slot =
            Operand{Mem{.base = Reg::Rsp,
                        .index = std::nullopt,
                        .scale = 1,
                        .disp = static_cast<i32>(stack_offset)}};
        push(make_inst(load, width_of(tag), slot, *side));
        stack_offset += 8;
        body_.outgoing = std::max(body_.outgoing, stack_offset);
      } else {
        push(make_inst(load, width_of(tag), Operand{ARG_XMM_REGS[next_xmm++]},
                       *side));
      }
      continue;
    }
    const Passage passage = passage_of(value.type);
    if (passage != Passage::Register) {
      if (passage == Passage::ByAddress) {
        if (next_register >= ARG_REG_COUNT) {
          if (!value.is<ir::RegisterIdx>()) {
            (void)unsupported(span, "a constant aggregate argument");
            return false;
          }
          // The callee reads a pointer from this slot, so spill the
          // address of the aggregate, not its first word's value.
          push(make_inst(
              MOp::Lea, 8, Operand{Reg::Rax},
              Operand{SlotRef{vreg_of(value.as_register(), value.type), 0}}));
          spill(Operand{Reg::Rax});
          continue;
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
        if (!value.is<ir::RegisterIdx>()) {
          const ir::Immutable& imm =
              storage_.immutables()[value.as_immutable()];
          const SymbolRef bytes = string_ref(rodata_.name(
              rodata_.intern(strings_.get(imm.data.str_id_value))));
          spill(Operand{bytes});
          spill(Operand{Imm{
              static_cast<i64>(strings_.get(imm.data.str_id_value).size())}});
        } else {
          const u32 vreg = vreg_of(value.as_register(), value.type);
          spill(Operand{SlotRef{vreg, 0}});
          spill(Operand{SlotRef{vreg, 8}});
        }
        continue;
      }
      if (!value.is<ir::RegisterIdx>()) {
        const ir::Immutable& imm = storage_.immutables()[value.as_immutable()];
        const SymbolRef bytes = string_ref(
            rodata_.name(rodata_.intern(strings_.get(imm.data.str_id_value))));
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
      spill(*side);
      continue;
    }
    push(make_inst(MOp::Mov, width_of(tag), Operand{ARG_REGS[next_register++]},
                   *side));
  }
  const bool sited =
      backtrace_ && current_instr_idx_.idx < site_names_.size() &&
      site_names_[current_instr_idx_.idx] != str::EMPTY_STRING_ID;
  if (sited && !placed_) {
    MInst block_mark = make_inst(MOp::Nop, 8);
    block_mark.label = label_;
    body_.insts.push_back(block_mark);
    placed_ = true;
  }
  const u32 call_label = sited ? fresh_label() : MInst::NO_LABEL;
  if (indirect) {
    const u32 vreg =
        vreg_of(operand_at(instr, 0).as_register(), operand_at(instr, 0).type);
    push(make_inst(MOp::Mov, 8, Operand{Reg::Rax}, Operand{SlotRef{vreg, 0}}));
    MInst call_inst = make_inst(MOp::CallReg, 8, Operand{Reg::Rax});
    call_inst.label = call_label;
    push(call_inst);
  } else {
    MInst call_inst = make_inst(MOp::Call, 8, Operand{SymbolRef{symbol_id}});
    call_inst.label = call_label;
    push(call_inst);
  }
  if (sited) {
    body_.call_sites.push_back(Body::CallSite{
        call_label,
        std::string(strings_.get(site_names_[current_instr_idx_.idx])),
        static_cast<u8>(indirect ? 2 : 5)});
  }
  if (defines_dst(instr)) {
    const Passage passage = passage_of(dst_type(instr));
    const ir::TypeTag returned = dst_value_tag(instr);
    if (is_float(returned)) {
      push(make_inst(returned == ir::TypeTag::F32 ? MOp::Movss : MOp::Movsd,
                     width_of(returned),
                     Operand{VReg{vreg_of(instr.dst, dst_type(instr))}},
                     Operand{RETURN_XMM_REG}));
    } else if (passage == Passage::Register) {
      push(make_inst(MOp::Mov, width_of(returned), dst_reg_operand(instr),
                     Operand{Reg::Rax}));
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
    const SelectContext& context) {
  Selector selector(context);
  return selector.run();
}

}  // namespace codegen::x86_64
