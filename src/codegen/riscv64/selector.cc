// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/selector.h"

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

#include "codegen/backend.h"
#include "codegen/diag_code.h"
#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/rodata.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "i18n/messages.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/parametric.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "overlay/overlay.h"

namespace codegen::riscv64 {

// A value that crosses a call in two integer registers: a string's
// pointer and length, a slice's base and count, a function's code and
// environment.
constexpr bool is_two_word(const ir::TypeTag tag) {
  return tag == ir::TypeTag::Str || tag == ir::TypeTag::Func ||
         tag == ir::TypeTag::Slice;
}
namespace {

constexpr bool is_aggregate_tag(const ir::TypeTag tag) {
  return tag == ir::TypeTag::Struct || tag == ir::TypeTag::Array ||
         tag == ir::TypeTag::Enum || tag == ir::TypeTag::Tuple;
}
enum class Passage : u8 { Register, TwoWord, ByAddress };

constexpr bool is_float_tag(const ir::TypeTag tag) {
  return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
}

constexpr bool is_integer_tag(const ir::TypeTag tag) {
  return tag == ir::TypeTag::I1 || tag == ir::TypeTag::I8 ||
         tag == ir::TypeTag::I16 || tag == ir::TypeTag::I32 ||
         tag == ir::TypeTag::I64 || tag == ir::TypeTag::Isize ||
         tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
         tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64 ||
         tag == ir::TypeTag::Usize || tag == ir::TypeTag::Ptr ||
         tag == ir::TypeTag::Char || tag == ir::TypeTag::Rune;
}

class Selector {
 public:
  explicit Selector(const SelectContext& context)
      : storage_(context.storage),
        strings_(context.strings),
        function_(context.function),
        spans_(context.spans),
        bag_(context.bag),
        width_(context.width),
        rodata_(context.rodata),
        overlay_(context.overlay),
        site_names_(context.instr_site_names),
        backtrace_(context.backtrace) {}

  base::Result<Body, EmitError> run();

 private:
  u32 vreg_of(ir::RegisterIdx reg, ir::TypeIdx type) {
    const auto found = vregs_.find(reg.idx);
    if (found != vregs_.end()) {
      return found->second;
    }
    const u32 index = static_cast<u32>(body_.vreg_types.size());
    vregs_[reg.idx] = index;
    body_.vreg_types.push_back(type.idx);
    const ir::TypeLayout layout = storage_.layout_of(type, width_);
    body_.vreg_sizes.push_back(static_cast<u32>(layout.size));
    const ir::TypeTag tag = storage_.types()[type.idx].tag;
    const bool unsig = tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                       tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64 ||
                       tag == ir::TypeTag::Usize || tag == ir::TypeTag::Char ||
                       tag == ir::TypeTag::Ptr;
    body_.vreg_signed.push_back(!unsig);
    if (alloca_regs_.contains(reg.idx)) {
      alloca_storage_.insert(index);
    }
    return index;
  }

  u32 vreg_of(const ir::Operand& operand) {
    return vreg_of(operand.as_register(), operand.type);
  }

  Operand dst_of(const ir::Instruction& instr) {
    return Operand{
        VReg{vreg_of(instr.dst, storage_.registers()[instr.dst].type)}};
  }

  const ir::Operand& at(const ir::Instruction& instr, const u32 index) {
    return storage_.operands()[instr.operands.head() + index];
  }

  std::optional<Operand> scalar_of(const ir::Operand& operand) {
    if (operand.is<ir::RegisterIdx>()) {
      return Operand{VReg{vreg_of(operand.as_register(), operand.type)}};
    }
    if (operand.is<ir::ImmutableIdx>()) {
      const ir::Immutable& value =
          storage_.immutables()[operand.as_immutable()];
      const ir::TypeTag tag = storage_.types()[operand.type.idx].tag;
      if (is_integer_tag(tag)) {
        return Operand{Imm{static_cast<i64>(value.as_u64_integer(tag))}};
      }
      if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
        const u64 bits = value.as_u64_bits(tag);
        const u32 bytes = tag == ir::TypeTag::F32 ? 4 : 8;
        const std::string data(reinterpret_cast<const char*>(&bits), bytes);
        const SymbolRef symbol = string_ref(rodata_.intern(data));
        // Materialize the literal through fixed integer/float scratch
        // registers so the allocator does not mistake the address temp
        // for a floating-point slot.
        // Alternate address/value registers so two literals in one
        // instruction do not overwrite each other.
        const Reg addr = float_literal_count_ % 2 == 0 ? T3 : Reg::X29;
        const Reg value_reg = float_literal_count_ % 2 == 0 ? Reg::F6 : Reg::F7;
        ++float_literal_count_;
        push(make_inst(MOp::LoadAddr, 8, Operand{addr}, Operand{symbol}));
        push(make_inst(bytes == 4 ? MOp::Flw : MOp::Fld, bytes,
                       Operand{value_reg},
                       Operand{Mem{.base = RegRef{addr}, .disp = 0}}));
        return Operand{value_reg};
      }
    }
    return std::nullopt;
  }

  Operand place_of(const ir::Operand& pointer) {
    const u32 vreg = vreg_of(pointer.as_register(), pointer.type);
    if (alloca_storage_.contains(vreg)) {
      return Operand{SlotRef{vreg, 0}};
    }
    // A pointer held in a register names memory through that register.
    return Operand{Mem{.base = RegRef{VReg{vreg}}, .disp = 0}};
  }

  void push(MInst inst) {
    // `label_` is already the label id assigned to this block in run().
    // A synthetic label set by a comparison must not be overwritten.
    if (!placed_) {
      if (inst.label == MInst::NO_LABEL) {
        inst.label = label_;
      } else {
        MInst boundary = make_inst(MOp::Nop, 8);
        boundary.label = label_;
        body_.insts.push_back(boundary);
      }
      placed_ = true;
    }
    body_.insts.push_back(inst);
  }

  void emit_move(Operand dst, Operand src, u8 width = 8) {
    if (std::holds_alternative<Imm>(src)) {
      push(make_inst(MOp::Addi, width, dst, Operand{ZERO}, src));
    } else {
      push(make_inst(MOp::Addi, width, dst, src, Operand{Imm{0}}));
    }
  }

  static MOp load_of(u8 width, ir::TypeTag tag = ir::TypeTag::I64) {
    const bool uns = ir::is_unsigned_integer_type(tag);
    switch (width) {
      case 1: return uns ? MOp::Lbu : MOp::Lb;
      case 2: return uns ? MOp::Lhu : MOp::Lh;
      case 4: return uns ? MOp::Lwu : MOp::Lw;
      default: return MOp::Ld;
    }
  }
  static MOp store_of(u8 width) {
    switch (width) {
      case 1: return MOp::Sb;
      case 2: return MOp::Sh;
      case 4: return MOp::Sw;
      default: return MOp::Sd;
    }
  }
  u8 store_width(const ir::Operand& value) {
    const ir::TypeLayout layout = storage_.layout_of(value.type, width_);
    return static_cast<u8>(std::min<u64>(layout.size, 8));
  }
  Passage passage_of(ir::TypeIdx type) const {
    const ir::TypeTag tag = storage_.types()[type.idx].tag;
    if (is_aggregate_tag(tag)) {
      return Passage::ByAddress;
    }
    if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
      const ir::TypeIdx pointee =
          storage_.ref_types()[storage_.types()[type.idx].as_ref()].pointee;
      if (storage_.types()[pointee.idx].tag == ir::TypeTag::Slice) {
        return Passage::TwoWord;
      }
    }
    if (is_two_word(tag)) {
      return Passage::TwoWord;
    }
    return Passage::Register;
  }
  static Operand shift_place(const Operand& place, i32 off) {
    if (const SlotRef* s = std::get_if<SlotRef>(&place)) {
      return Operand{SlotRef{s->vreg, s->offset + off}};
    }
    if (const Mem* m = std::get_if<Mem>(&place)) {
      return Operand{Mem{.base = m->base, .disp = m->disp + off}};
    }
    return place;
  }
  // Materialize an address into a register. A displacement too large for
  // a single immediate is added as a separately built word; leaving it in
  // the memory operand would truncate it into unrelated instruction bits.
  void emit_address(Reg out, const Operand& place, Reg temp) {
    if (const SlotRef* slot = std::get_if<SlotRef>(&place)) {
      push(make_inst(MOp::Addr, 8, Operand{out}, Operand{*slot}));
      return;
    }
    Operand base = Operand{ZERO};
    i64 disp = 0;
    if (const Mem* mem = std::get_if<Mem>(&place)) {
      disp = mem->disp;
      if (mem->base.has_value()) {
        if (const Reg* reg = std::get_if<Reg>(&*mem->base)) {
          base = Operand{*reg};
        } else if (const VReg* vreg = std::get_if<VReg>(&*mem->base)) {
          base = Operand{*vreg};
        }
      }
    } else if (const VReg* vreg = std::get_if<VReg>(&place)) {
      base = Operand{*vreg};
    } else if (const Reg* reg = std::get_if<Reg>(&place)) {
      base = Operand{*reg};
    }
    if (disp >= -2048 && disp <= 2047) {
      push(make_inst(MOp::Addi, 8, Operand{out}, base, Operand{Imm{disp}}));
      return;
    }
    const i64 hi = disp >= 0 ? (disp + 2048) / 4096 : -((-disp + 2047) / 4096);
    const i64 lo = disp - hi * 4096;
    push(make_inst(MOp::Lui, 8, Operand{temp}, Operand{Imm{hi << 12}}));
    push(make_inst(MOp::Addi, 8, Operand{temp}, Operand{temp},
                   Operand{Imm{lo}}));
    push(make_inst(MOp::Add, 8, Operand{out}, base, Operand{temp}));
  }

  void emit_copy(const Operand& dst, const Operand& src, u64 size) {
    constexpr Reg dst_cursor = Reg::X22;
    constexpr Reg src_cursor = Reg::X23;
    constexpr Reg word = Reg::X26;
    constexpr Reg temp = Reg::X27;
    emit_address(src_cursor, src, temp);
    emit_address(dst_cursor, dst, temp);
    u64 off = 0;
    for (; off + 8 <= size; off += 8) {
      push(make_inst(MOp::Ld, 8, Operand{word},
                     Operand{Mem{.base = RegRef{src_cursor}, .disp = 0}}));
      push(make_inst(MOp::Sd, 8, Operand{word},
                     Operand{Mem{.base = RegRef{dst_cursor}, .disp = 0}}));
      push(make_inst(MOp::Addi, 8, Operand{src_cursor}, Operand{src_cursor},
                     Operand{Imm{8}}));
      push(make_inst(MOp::Addi, 8, Operand{dst_cursor}, Operand{dst_cursor},
                     Operand{Imm{8}}));
    }
    for (; off < size; ++off) {
      push(make_inst(MOp::Lbu, 1, Operand{word},
                     Operand{Mem{.base = RegRef{src_cursor}, .disp = 0}}));
      push(make_inst(MOp::Sb, 1, Operand{word},
                     Operand{Mem{.base = RegRef{dst_cursor}, .disp = 0}}));
      push(make_inst(MOp::Addi, 8, Operand{src_cursor}, Operand{src_cursor},
                     Operand{Imm{1}}));
      push(make_inst(MOp::Addi, 8, Operand{dst_cursor}, Operand{dst_cursor},
                     Operand{Imm{1}}));
    }
  }

  // Store a word (address or immediate) at a displacement from a place.
  void emit_store_addr(const Operand& place,
                       const i32 disp,
                       const Operand& value) {
    if (const SlotRef* slot = std::get_if<SlotRef>(&place); slot != nullptr) {
      push(make_inst(MOp::Sd, 8, value,
                     Operand{SlotRef{slot->vreg, slot->offset + disp}}));
    } else if (const Mem* mem = std::get_if<Mem>(&place); mem != nullptr) {
      push(
          make_inst(MOp::Sd, 8, value,
                    Operand{Mem{.base = mem->base, .disp = mem->disp + disp}}));
    } else if (const VReg* vreg = std::get_if<VReg>(&place); vreg != nullptr) {
      push(make_inst(MOp::Sd, 8, value,
                     Operand{Mem{.base = RegRef{*vreg}, .disp = disp}}));
    }
  }

  // A typed projection: operands are [base, 0, index...]. Answer the
  // byte displacement from the base and the type of the element reached.
  i64 projection_offset(const ir::Instruction& instr,
                        const u32 count,
                        ir::TypeIdx& type) {
    i64 offset = 0;
    for (u32 idx = 1; idx < count; ++idx) {
      const ir::Operand& step = at(instr, idx);
      if (!step.is<ir::ImmutableIdx>()) {
        return 0;
      }
      const ir::Immutable& imm = storage_.immutables()[step.as_immutable()];
      const i64 value = static_cast<i64>(
          imm.as_u64_integer(storage_.types()[step.type.idx].tag));
      if (idx == 1) {
        // The first index steps over the base object itself; it is zero.
        if (value != 0) {
          return 0;
        }
        // A reference's first projection dereferences it, so later
        // indices step over the pointee's fields, not the pointer word.
        const ir::TypeTag base = storage_.types()[type.idx].tag;
        if (base == ir::TypeTag::Ref || base == ir::TypeTag::MutRef ||
            base == ir::TypeTag::Ptr || base == ir::TypeTag::RawPtr ||
            base == ir::TypeTag::RawMutPtr) {
          type =
              storage_.ref_types()[storage_.types()[type.idx].as_ref()].pointee;
        }
        continue;
      }
      const ir::TypeTag tag = storage_.types()[type.idx].tag;
      if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Tuple) {
        const ir::TypeIdxRange fields =
            tag == ir::TypeTag::Struct
                ? storage_
                      .struct_types()[storage_.types()[type.idx].as_struct()]
                      .fields
                : storage_.tuple_types()[storage_.types()[type.idx].as_tuple()]
                      .elements;
        if (value < 0 || static_cast<u32>(value) >= fields.size()) {
          return 0;
        }
        const auto hints =
            tag == ir::TypeTag::Struct
                ? ir::field_hints_of(
                      storage_.state(),
                      storage_.struct_types()[storage_.types()[type.idx]
                                                  .as_struct()])
                : std::pair<const ir::TypeLayoutHints*, u32>{nullptr, 0};
        offset += static_cast<i64>(
            ir::field_offset(storage_.state(), fields, static_cast<u32>(value),
                             width_, hints.first, hints.second));
        type = fields[value];
      } else if (tag == ir::TypeTag::Array) {
        const ir::ArrayType& array =
            storage_.array_types()[storage_.types()[type.idx].as_array()];
        offset += static_cast<i64>(
            storage_.layout_of(array.element, width_).size * value);
        type = array.element;
      } else if (passage_of(type) == Passage::TwoWord) {
        // A fat pointer (str, slice) is projected one eight-byte word
        // at a time, not by its whole sixteen-byte layout size.
        offset += value * 8;
      } else {
        offset +=
            static_cast<i64>(storage_.layout_of(type, width_).size) * value;
      }
    }
    return offset;
  }

  bool emit_inner(const ir::Instruction& instr,
                  const diag::Span span,
                  const overlay::Entry* check);
  void emit_panic_call(std::string_view message, str::StringPoolId site);
  bool emit_check_prelude(const ir::Instruction& instr,
                          const overlay::Entry& check);
  bool emit_overflow_check(const ir::Instruction& instr,
                           const overlay::Entry& check,
                           Operand lhs,
                           Operand rhs);
  [[nodiscard]] SymbolRef string_ref(const std::string& symbol);
  bool unsupported(diag::Span span, std::string_view what);
  bool emit_call(const ir::Instruction& instr, const diag::Span span);

  const ir::Storage& storage_;
  const ir::SymbolTable& strings_;
  const ir::Function& function_;
  std::span<const diag::Span> spans_;
  diag::DiagBag& bag_;
  ir::PointerWidth width_;
  RoData& rodata_;
  const overlay::Overlay* overlay_ = nullptr;
  std::span<const str::StringPoolId> site_names_;
  bool backtrace_ = false;
  bool failed_ = false;
  ir::InstructionIdx current_instr_idx_ = ir::InstructionIdx::invalid();
  Body body_;
  std::unordered_map<u32, u32> vregs_;
  std::unordered_set<u32> alloca_storage_;
  std::unordered_set<u32> alloca_regs_;
  std::unordered_map<u32, ir::TypeIdx> pointee_of_vreg_;
  std::unordered_map<u32, u32> block_labels_;
  u32 label_ = 0;
  bool placed_ = false;
  u32 return_label_ = MInst::NO_LABEL;
  u32 float_literal_count_ = 0;
  static constexpr u32 NO_SRET = 0xffffffff;
  u32 sret_vreg_ = NO_SRET;
};

base::Result<Body, EmitError> Selector::run() {
  // Blocks need not be laid out in dominance order. Note every alloca
  // register up front so a use in an earlier-laid-out block still marks
  // its vreg as slot-backed when vreg_of first creates it.
  for (const ir::BlockIdx block : function_.blocks) {
    for (const ir::InstructionIdx instr_idx : storage_.blocks()[block].instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      if (instr.op == ir::Opcode::Alloca) {
        alloca_regs_.insert(instr.dst.idx);
      }
    }
  }
  for (const ir::BlockIdx block : function_.blocks) {
    block_labels_[block.idx] = body_.label_count++;
  }
  return_label_ = body_.label_count++;
  const ir::TypeTag returned =
      storage_.types()[function_.meta.return_type.idx].tag;
  if (is_aggregate_tag(returned)) {
    body_.uses_sret = true;
    sret_vreg_ = static_cast<u32>(body_.vreg_types.size());
    body_.vreg_types.push_back(ir::primitive_idx(ir::TypeTag::Ptr).idx);
    body_.vreg_sizes.push_back(8);
    body_.vreg_signed.push_back(false);
  }
  bool entry = true;
  for (const ir::BlockIdx block_idx : function_.blocks) {
    label_ = block_labels_[block_idx.idx];
    placed_ = false;
    const ir::Block& block = storage_.blocks()[block_idx];
    if (entry) {
      entry = false;
      u32 arg = 0;
      u32 next_float = 0;
      if (sret_vreg_ != NO_SRET) {
        emit_move(Operand{S0}, Operand{ARG_REGS[arg++]});
      }
      for (const ir::BlockParamIdx param_idx : block.block_params) {
        const ir::BlockParam& param = storage_.block_params()[param_idx];
        const u32 vreg = vreg_of(param.reg, param.type);
        const ir::TypeTag tag = storage_.types()[param.type.idx].tag;
        if (passage_of(param.type) == Passage::ByAddress) {
          if (arg >= ARG_REG_COUNT) {
            break;
          }
          alloca_storage_.insert(vreg);
          emit_copy(Operand{SlotRef{vreg, 0}},
                    Operand{Mem{.base = RegRef{ARG_REGS[arg]}, .disp = 0}},
                    storage_.layout_of(param.type, width_).size);
          ++arg;
          continue;
        }
        if (is_float_tag(tag)) {
          if (next_float >= ARG_FREG_COUNT) {
            break;
          }
          const u8 w = tag == ir::TypeTag::F32 ? 4 : 8;
          push(make_inst(w == 4 ? MOp::Fsw : MOp::Fsd, w,
                         Operand{ARG_FREGS[next_float]},
                         Operand{SlotRef{vreg, 0}}));
          ++next_float;
          continue;
        }
        if (passage_of(param.type) == Passage::TwoWord) {
          if (arg + 1 >= ARG_REG_COUNT) {
            break;
          }
          // The words live in this vreg's own slot; a later projection
          // reads the slot rather than dereferencing the first word.
          alloca_storage_.insert(vreg);
          push(make_inst(MOp::Sd, 8, Operand{ARG_REGS[arg++]},
                         Operand{SlotRef{vreg, 0}}));
          push(make_inst(MOp::Sd, 8, Operand{ARG_REGS[arg++]},
                         Operand{SlotRef{vreg, 8}}));
        } else {
          if (arg >= ARG_REG_COUNT) {
            break;
          }
          emit_move(Operand{VReg{vreg}}, Operand{ARG_REGS[arg++]});
        }
      }
    }
    for (const ir::InstructionIdx instr_idx : block.instrs) {
      const ir::Instruction& instr = storage_.instrs()[instr_idx];
      const diag::Span span =
          instr_idx.idx < spans_.size() ? spans_[instr_idx.idx] : diag::Span{};
      const overlay::Entry* check =
          overlay_ == nullptr || instr_idx.idx >= overlay_->entries.size()
              ? nullptr
              : &overlay_->entries[instr_idx.idx];
      current_instr_idx_ = instr_idx;
      if (!emit_inner(instr, span, check)) {
        return base::make_err(EmitError::Unsupported);
      }
    }
    if (!placed_) {
      // Instructions that lower to no machine words still need a label
      // so branches to this block do not land in the next one.
      push(make_inst(MOp::Nop, 8));
    }
  }
  MInst at_return = make_inst(MOp::Nop, 8);
  at_return.label = return_label_;
  push(at_return);
  return base::make_ok(std::move(body_));
}

MOp alu_of(const ir::Opcode op, bool& immediate) {
  immediate = true;
  switch (op) {
    case ir::Opcode::IntAdd:
    case ir::Opcode::IntAddWrap: return MOp::Add;
    case ir::Opcode::IntSub:
    case ir::Opcode::IntSubWrap: return MOp::Sub;
    case ir::Opcode::IntMul:
    case ir::Opcode::IntMulWrap: return MOp::Mul;
    case ir::Opcode::IntDiv: return MOp::Div;
    case ir::Opcode::UintDiv: return MOp::Divu;
    case ir::Opcode::IntRem: return MOp::Rem;
    case ir::Opcode::UintRem: return MOp::Remu;
    case ir::Opcode::And: return MOp::And;
    case ir::Opcode::Or: return MOp::Or;
    case ir::Opcode::Xor: return MOp::Xor;
    case ir::Opcode::ShiftLeft: return MOp::Sll;
    case ir::Opcode::ArithmeticShiftRight: return MOp::Sra;
    case ir::Opcode::LogicalShiftRight: return MOp::Srl;
    default: immediate = false; return MOp::Nop;
  }
}

i64 int_bits_of(const ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::U8:
    case ir::TypeTag::Char: return 8;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 16;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32:
    case ir::TypeTag::Rune: return 32;
    default: return 64;
  }
}

void Selector::emit_panic_call(const std::string_view message,
                               const str::StringPoolId site) {
  const SymbolRef text = string_ref(rodata_.intern(message));
  push(make_inst(MOp::LoadAddr, 8, Operand{A0}, Operand{text}));
  emit_move(Operand{A1}, Operand{Imm{static_cast<i64>(message.size())}});
  if (site == str::EMPTY_STRING_ID) {
    emit_move(Operand{A2}, Operand{ZERO});
    emit_move(Operand{A3}, Operand{ZERO});
  } else {
    const std::string_view where = strings_.get(site);
    const SymbolRef name = string_ref(rodata_.intern(where));
    push(make_inst(MOp::LoadAddr, 8, Operand{A2}, Operand{name}));
    emit_move(Operand{A3}, Operand{Imm{static_cast<i64>(where.size())}});
  }
  const SymbolRef panic = string_ref("alcy_panic");
  push(make_inst(MOp::Auipc, 8, Operand{T0}, Operand{Imm{0}}));
  push(make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0},
                 Operand{SymbolRef{panic.id}}));
  push(make_inst(MOp::Jalr, 8, Operand{RA}, Operand{T0}, Operand{Imm{0}}));
}

bool Selector::emit_check_prelude(const ir::Instruction& instr,
                                  const overlay::Entry& check) {
  if (check.kind == overlay::Check::ShiftRange) {
    const ir::Operand& raw_op = check.operand.is_valid()
                                    ? storage_.operands()[check.operand]
                                    : at(instr, 1);
    auto raw = scalar_of(raw_op);
    if (!raw.has_value()) {
      return false;
    }
    const ir::TypeTag tag = storage_.types()[at(instr, 0).type.idx].tag;
    const i64 bits = int_bits_of(tag);
    const u32 ok = body_.label_count++;
    emit_move(Operand{Reg::X22}, *raw);
    emit_move(Operand{Reg::X23}, Operand{Imm{bits}});
    MInst branch = make_inst(MOp::Bltu, 8, Operand{Reg::X22}, Operand{Reg::X23},
                             Operand{Label{ok}});
    branch.cond = Cond::Ltu;
    push(branch);
    emit_panic_call("shift out of range", check.site);
    MInst at_ok = make_inst(MOp::Nop, 8);
    at_ok.label = ok;
    push(at_ok);
  } else if (check.kind == overlay::Check::MinOverOne) {
    auto dividend = scalar_of(at(instr, 0));
    const ir::Operand& divisor_op = check.operand.is_valid()
                                        ? storage_.operands()[check.operand]
                                        : at(instr, 1);
    auto divisor = scalar_of(divisor_op);
    if (!dividend.has_value() || !divisor.has_value()) {
      return false;
    }
    const ir::TypeTag tag = storage_.types()[at(instr, 0).type.idx].tag;
    const i64 minimum = tag == ir::TypeTag::I8    ? -128
                        : tag == ir::TypeTag::I16 ? -32768
                        : tag == ir::TypeTag::I32 ? INT32_MIN
                                                  : INT64_MIN;
    const u32 ok = body_.label_count++;
    emit_move(Operand{Reg::X22}, *dividend);
    emit_move(Operand{Reg::X23}, Operand{Imm{minimum}});
    MInst not_min = make_inst(MOp::Bne, 8, Operand{Reg::X22}, Operand{Reg::X23},
                              Operand{Label{ok}});
    not_min.cond = Cond::Ne;
    push(not_min);
    emit_move(Operand{Reg::X22}, *divisor);
    emit_move(Operand{Reg::X23}, Operand{Imm{-1}});
    MInst not_one = make_inst(MOp::Bne, 8, Operand{Reg::X22}, Operand{Reg::X23},
                              Operand{Label{ok}});
    not_one.cond = Cond::Ne;
    push(not_one);
    emit_panic_call("integer overflow", check.site);
    MInst at_ok = make_inst(MOp::Nop, 8);
    at_ok.label = ok;
    push(at_ok);
  }
  return true;
}

bool Selector::emit_overflow_check(const ir::Instruction& instr,
                                   const overlay::Entry& check,
                                   Operand lhs,
                                   Operand rhs) {
  const ir::TypeTag tag = storage_.types()[at(instr, 0).type.idx].tag;
  const bool uns = ir::is_unsigned_integer_type(tag);
  const i64 bits = int_bits_of(tag);
  const u32 ok = body_.label_count++;
  constexpr Reg result = Reg::X22;
  constexpr Reg left = Reg::X23;
  constexpr Reg right = Reg::X25;
  constexpr Reg temp = Reg::X26;
  // Recompute the full-width result here: a narrow result read back from
  // its slot is zero-extended and would fail a sign-extension check.
  bool immediate = false;
  const MOp alu = alu_of(instr.op, immediate);
  emit_move(Operand{left}, lhs);
  emit_move(Operand{right}, rhs);
  push(make_inst(alu, 8, Operand{result}, Operand{left}, Operand{right}));
  MInst to_ok;
  if (!uns && bits < 64) {
    // The wrapped result must be the sign extension of its low `bits`.
    push(make_inst(MOp::Slli, 8, Operand{temp}, Operand{result},
                   Operand{Imm{64 - bits}}));
    push(make_inst(MOp::Srai, 8, Operand{temp}, Operand{temp},
                   Operand{Imm{64 - bits}}));
    to_ok = make_inst(MOp::Beq, 8, Operand{temp}, Operand{result},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Eq;
  } else if (!uns && instr.op == ir::Opcode::IntMul) {
    // A multiply overflows when the high half of the product differs
    // from the sign extension of the low half.
    push(make_inst(MOp::Mulh, 8, Operand{temp}, Operand{left}, Operand{right}));
    push(make_inst(MOp::Srai, 8, Operand{left}, Operand{result},
                   Operand{Imm{63}}));
    to_ok = make_inst(MOp::Beq, 8, Operand{temp}, Operand{left},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Eq;
  } else if (!uns) {
    // Addition overflows when the operands share a sign the result
    // lacks; subtraction overflows when they differ in sign from it.
    if (instr.op == ir::Opcode::IntSub) {
      push(
          make_inst(MOp::Xor, 8, Operand{temp}, Operand{left}, Operand{right}));
      push(make_inst(MOp::Xor, 8, Operand{left}, Operand{left},
                     Operand{result}));
    } else {
      push(make_inst(MOp::Xor, 8, Operand{temp}, Operand{left},
                     Operand{result}));
      push(make_inst(MOp::Xor, 8, Operand{left}, Operand{right},
                     Operand{result}));
    }
    push(make_inst(MOp::And, 8, Operand{temp}, Operand{temp}, Operand{left}));
    to_ok = make_inst(MOp::Blt, 8, Operand{temp}, Operand{ZERO},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Ge;
  } else if (bits < 64) {
    // An unsigned result fits its width when the high bits are clear.
    push(make_inst(MOp::Srli, 8, Operand{temp}, Operand{result},
                   Operand{Imm{bits}}));
    to_ok = make_inst(MOp::Beq, 8, Operand{temp}, Operand{ZERO},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Eq;
  } else if (instr.op == ir::Opcode::IntMul) {
    // An unsigned product overflows when its high word is non-zero.
    push(
        make_inst(MOp::Mulhu, 8, Operand{temp}, Operand{left}, Operand{right}));
    to_ok = make_inst(MOp::Beq, 8, Operand{temp}, Operand{ZERO},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Eq;
  } else if (instr.op == ir::Opcode::IntSub) {
    // Wrapping subtraction underflows when the result exceeds the left
    // operand as an unsigned value.
    to_ok = make_inst(MOp::Bgeu, 8, Operand{left}, Operand{result},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Geu;
  } else {
    // Addition wraps only when the result drops below the left operand.
    to_ok = make_inst(MOp::Bgeu, 8, Operand{result}, Operand{left},
                      Operand{Label{ok}});
    to_ok.cond = Cond::Geu;
  }
  push(to_ok);
  emit_panic_call("integer overflow", check.site);
  MInst at_ok = make_inst(MOp::Nop, 8);
  at_ok.label = ok;
  push(at_ok);
  return true;
}

bool Selector::emit_inner(const ir::Instruction& instr,
                          const diag::Span span,
                          const overlay::Entry* check) {
  using O = ir::Opcode;
  switch (instr.op) {
    case O::Noop:
    case O::Drop:
    case O::ParametricDrop: return true;
    case O::Move: {
      // A bare `move` (no destination) is a lifetime mark, like a Drop.
      if (!instr.dst.is_valid()) {
        return true;
      }
      auto src = scalar_of(at(instr, 0));
      if (!src.has_value()) {
        return false;
      }
      const u64 size =
          storage_.layout_of(storage_.registers()[instr.dst].type, width_).size;
      if (size > 8 && at(instr, 0).is<ir::RegisterIdx>()) {
        const u32 dst =
            vreg_of(instr.dst, storage_.registers()[instr.dst].type);
        const u32 from = vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
        alloca_storage_.insert(dst);
        emit_copy(Operand{SlotRef{dst, 0}}, Operand{SlotRef{from, 0}}, size);
      } else {
        emit_move(dst_of(instr), *src);
      }
      return true;
    }
    case O::Borrow: {
      if (!at(instr, 0).is<ir::RegisterIdx>()) {
        return false;
      }
      const u32 vreg = vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
      pointee_of_vreg_.insert_or_assign(
          vreg_of(instr.dst, storage_.registers()[instr.dst].type),
          pointee_of_vreg_.contains(vreg)
              ? pointee_of_vreg_.at(vreg)
              : storage_.registers()[at(instr, 0).as_register()].type);
      if (alloca_storage_.contains(vreg)) {
        push(make_inst(MOp::Addr, 8, dst_of(instr), Operand{SlotRef{vreg, 0}}));
      } else {
        // The source vreg already holds the address. place_of would turn
        // it into a Mem, which emit_move cannot encode as a copy.
        emit_move(dst_of(instr), Operand{VReg{vreg}});
      }
      return true;
    }
    case O::Ret: {
      if (instr.operands.size() != 0) {
        const ir::TypeTag ret_tag = storage_.types()[at(instr, 0).type.idx].tag;
        if (sret_vreg_ != NO_SRET && ret_tag != ir::TypeTag::Str &&
            at(instr, 0).is<ir::RegisterIdx>()) {
          const u32 vreg =
              vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
          emit_copy(Operand{Mem{.base = RegRef{S0}, .disp = 0}},
                    Operand{SlotRef{vreg, 0}},
                    storage_.layout_of(at(instr, 0).type, width_).size);
        } else if (ret_tag == ir::TypeTag::Str &&
                   at(instr, 0).is<ir::ImmutableIdx>()) {
          const ir::Immutable& imm =
              storage_.immutables()[at(instr, 0).as_immutable()];
          const std::string_view text = strings_.get(imm.data.str_id_value);
          const SymbolRef symbol = string_ref(rodata_.intern(text));
          push(make_inst(MOp::LoadAddr, 8, Operand{A0}, Operand{symbol}));
          emit_move(Operand{A1}, Operand{Imm{static_cast<i64>(text.size())}});
        } else if (passage_of(at(instr, 0).type) == Passage::TwoWord &&
                   at(instr, 0).is<ir::RegisterIdx>()) {
          const u32 vreg =
              vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
          push(make_inst(MOp::Ld, 8, Operand{A0}, Operand{SlotRef{vreg, 0}}));
          push(make_inst(MOp::Ld, 8, Operand{A1}, Operand{SlotRef{vreg, 8}}));
        } else if (is_float_tag(ret_tag) &&
                   at(instr, 0).is<ir::RegisterIdx>()) {
          const u8 w = ret_tag == ir::TypeTag::F32 ? 4 : 8;
          const u32 vreg =
              vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
          push(make_inst(w == 4 ? MOp::Flw : MOp::Fld, w, Operand{Reg::F10},
                         Operand{SlotRef{vreg, 0}}));
        } else {
          auto value = scalar_of(at(instr, 0));
          if (!value.has_value()) {
            return false;
          }
          if (is_float_tag(ret_tag)) {
            // A float literal materializes into a physical FPR; a GPR
            // move would read its number as a general-purpose register.
            const u8 float_width = ret_tag == ir::TypeTag::F32 ? 4 : 8;
            push(make_inst(MOp::Fmov, float_width, Operand{Reg::F10}, *value));
          } else {
            emit_move(Operand{A0}, *value);
          }
        }
      }
      push(
          make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{return_label_}}));
      return true;
    }
    case O::Br: {
      const ir::BlockIdx target = at(instr, 0).as_block();
      const ir::Block& destination = storage_.blocks()[target];
      if (instr.operands.size() != 1 + destination.block_params.size()) {
        return false;
      }
      for (u32 param = 0; param + 1 < instr.operands.size(); ++param) {
        const ir::BlockParam& block_param =
            storage_.block_params()[destination.block_params[param]];
        const u32 dst = vreg_of(block_param.reg, block_param.type);
        const u64 size = storage_.layout_of(block_param.type, width_).size;
        auto value = scalar_of(at(instr, param + 1));
        if (!value.has_value()) {
          return false;
        }
        if (size > 8 && at(instr, param + 1).is<ir::RegisterIdx>()) {
          const u32 src = vreg_of(at(instr, param + 1).as_register(),
                                  at(instr, param + 1).type);
          alloca_storage_.insert(dst);
          emit_copy(Operand{SlotRef{dst, 0}}, Operand{SlotRef{src, 0}}, size);
        } else {
          emit_move(Operand{VReg{dst}}, *value);
        }
      }
      push(make_inst(MOp::Jal, 8, Operand{ZERO},
                     Operand{Label{block_labels_[target.idx]}}));
      return true;
    }
    case O::Switch: {
      // operands = [value, default, (case_imm, case_block)...].
      if (instr.operands.size() < 2) {
        return false;
      }
      auto value = scalar_of(at(instr, 0));
      if (!value.has_value()) {
        return false;
      }
      for (u32 idx = 2; idx + 1 < instr.operands.size(); idx += 2) {
        auto case_value = scalar_of(at(instr, idx));
        if (!case_value.has_value()) {
          return false;
        }
        const ir::BlockIdx target = at(instr, idx + 1).as_block();
        MInst branch = make_inst(MOp::Beq, 8, *value, *case_value,
                                 Operand{Label{block_labels_[target.idx]}});
        branch.cond = Cond::Eq;
        push(branch);
      }
      const ir::BlockIdx fallback = at(instr, 1).as_block();
      push(make_inst(MOp::Jal, 8, Operand{ZERO},
                     Operand{Label{block_labels_[fallback.idx]}}));
      return true;
    }
    case O::CondBr: {
      auto condition = scalar_of(at(instr, 0));
      if (!condition.has_value()) {
        return false;
      }
      const ir::BlockIdx yes = at(instr, 1).as_block();
      const ir::BlockIdx no = at(instr, 2).as_block();
      MInst branch = make_inst(MOp::Bne, 8, *condition, Operand{ZERO},
                               Operand{Label{block_labels_[yes.idx]}});
      branch.cond = Cond::Ne;
      push(branch);
      push(make_inst(MOp::Jal, 8, Operand{ZERO},
                     Operand{Label{block_labels_[no.idx]}}));
      return true;
    }
    case O::Unreachable: {
      push(make_inst(MOp::Ecall, 8));
      return true;
    }
    default: break;
  }

  bool immediate = false;
  const MOp alu = alu_of(instr.op, immediate);
  if (immediate) {
    if (check != nullptr && check->kind != overlay::Check::None &&
        check->kind != overlay::Check::Overflow &&
        !emit_check_prelude(instr, *check)) {
      return false;
    }
    auto lhs = scalar_of(at(instr, 0));
    auto rhs = scalar_of(at(instr, 1));
    if (!lhs.has_value() || !rhs.has_value()) {
      return false;
    }
    push(make_inst(alu, 8, dst_of(instr), *lhs, *rhs));
    if (check != nullptr && check->kind == overlay::Check::Overflow &&
        !emit_overflow_check(instr, *check, *lhs, *rhs)) {
      return false;
    }
    return true;
  }

  if (instr.op == O::Not) {
    auto value = scalar_of(at(instr, 0));
    if (!value.has_value()) {
      return false;
    }
    const ir::TypeTag tag =
        storage_.types()[storage_.registers()[instr.dst].type.idx].tag;
    // Logical not of a bool flips its single bit; a bitwise complement
    // of any wider integer flips every bit.
    const i64 mask = tag == ir::TypeTag::I1 ? 1 : -1;
    push(make_inst(MOp::Xori, 8, dst_of(instr), *value, Operand{Imm{mask}}));
    return true;
  }

  if (instr.op == O::Eq || instr.op == O::Ne || instr.op == O::Lt ||
      instr.op == O::Le || instr.op == O::Gt || instr.op == O::Ge) {
    auto lhs = scalar_of(at(instr, 0));
    auto rhs = scalar_of(at(instr, 1));
    if (!lhs.has_value() || !rhs.has_value()) {
      return false;
    }
    const ir::TypeTag lhs_tag = storage_.types()[at(instr, 0).type.idx].tag;
    if (is_float_tag(lhs_tag)) {
      const u8 w = lhs_tag == ir::TypeTag::F32 ? 4 : 8;
      const Reg tmp = Reg::X25;  // outside the allocator scratch pool
      MOp fcmp = MOp::Feq;
      bool swap = false;
      bool invert = false;
      switch (instr.op) {
        case O::Eq: fcmp = MOp::Feq; break;
        case O::Ne:
          fcmp = MOp::Feq;
          invert = true;
          break;
        case O::Lt: fcmp = MOp::Flt; break;
        case O::Le: fcmp = MOp::Fle; break;
        case O::Gt:
          fcmp = MOp::Flt;
          swap = true;
          break;
        case O::Ge:
          fcmp = MOp::Fle;
          swap = true;
          break;
        default: break;
      }
      if (swap) {
        push(make_inst(fcmp, w, Operand{tmp}, *rhs, *lhs));
      } else {
        push(make_inst(fcmp, w, Operand{tmp}, *lhs, *rhs));
      }
      if (invert) {
        push(make_inst(MOp::Xori, 8, dst_of(instr), Operand{tmp},
                       Operand{Imm{1}}));
      } else {
        emit_move(dst_of(instr), Operand{tmp});
      }
      return true;
    }
    const bool uns = !ir::is_signed_integer_type(lhs_tag);
    const Cond lt = uns ? Cond::Ltu : Cond::Lt;
    const Cond ge = uns ? Cond::Geu : Cond::Ge;
    Cond cond = Cond::Eq;
    bool swap = false;
    switch (instr.op) {
      case O::Eq: cond = Cond::Eq; break;
      case O::Ne: cond = Cond::Ne; break;
      case O::Lt: cond = lt; break;
      case O::Le:
        cond = ge;
        swap = true;
        break;
      case O::Gt:
        cond = lt;
        swap = true;
        break;
      case O::Ge: cond = ge; break;
      default: break;
    }
    const u32 yes = body_.label_count++;
    const u32 done = body_.label_count++;
    MInst branch = make_inst(MOp::Beq, 8, swap ? *rhs : *lhs,
                             swap ? *lhs : *rhs, Operand{Label{yes}});
    branch.cond = cond;
    push(branch);
    emit_move(dst_of(instr), Operand{ZERO});
    push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
    MInst at_yes = make_inst(MOp::Nop, 8);
    at_yes.label = yes;
    push(at_yes);
    emit_move(dst_of(instr), Operand{Imm{1}});
    MInst at_done = make_inst(MOp::Nop, 8);
    at_done.label = done;
    push(at_done);
    (void)span;
    return true;
  }

  if (instr.op == O::TypeSizeOf || instr.op == O::TypeAlignOf) {
    const ir::TypeLayout layout = storage_.layout_of(instr.measure, width_);
    const u64 value = instr.op == O::TypeSizeOf ? layout.size : layout.align;
    emit_move(dst_of(instr), Operand{Imm{static_cast<i64>(value)}});
    return true;
  }

  if (instr.op == O::Alloca) {
    // The frame slot the vreg names is the storage; no instruction is
    // needed here. Loads and stores through it resolve to the slot.
    const u32 vreg = vreg_of(instr.dst, storage_.registers()[instr.dst].type);
    alloca_storage_.insert(vreg);
    pointee_of_vreg_.insert_or_assign(vreg,
                                      storage_.registers()[instr.dst].type);
    return true;
  }

  if (instr.op == O::Load) {
    if (!at(instr, 0).is<ir::RegisterIdx>()) {
      return false;
    }
    const u64 size =
        storage_.layout_of(storage_.registers()[instr.dst].type, width_).size;
    if (size == 0) {
      // A zero-sized value has no storage; reading it is a no-op.
      return true;
    }
    if (size > 8) {
      const u32 dst_vreg =
          vreg_of(instr.dst, storage_.registers()[instr.dst].type);
      // The whole aggregate now lives in this vreg's slot; mark it so
      // projections and place_of read the slot instead of treating the
      // first word as a pointer to the value.
      alloca_storage_.insert(dst_vreg);
      emit_copy(Operand{SlotRef{dst_vreg, 0}}, place_of(at(instr, 0)), size);
      return true;
    }
    const u8 w = static_cast<u8>(size);
    const ir::TypeTag loaded_tag =
        storage_.types()[storage_.registers()[instr.dst].type.idx].tag;
    const MOp load = is_float_tag(loaded_tag) ? (w == 4 ? MOp::Flw : MOp::Fld)
                                              : load_of(w, loaded_tag);
    push(make_inst(load, w, dst_of(instr), place_of(at(instr, 0))));
    return true;
  }

  if (instr.op == O::Store) {
    // IR order is [value, ptr].
    const ir::Operand& value_op = at(instr, 0);
    if (!at(instr, 1).is<ir::RegisterIdx>()) {
      return false;
    }
    // A string literal is two words: the address of its bytes and the
    // byte count. Store both into the destination slot.
    if (value_op.is<ir::ImmutableIdx>() &&
        storage_.types()[value_op.type.idx].tag == ir::TypeTag::Str) {
      const ir::Immutable& imm = storage_.immutables()[value_op.as_immutable()];
      const std::string_view text = strings_.get(imm.data.str_id_value);
      const SymbolRef symbol = string_ref(rodata_.intern(text));
      const Operand dst = place_of(at(instr, 1));
      // The allocator cannot materialize a SymbolRef as a store value;
      // materialize the literal's address into a scratch register first.
      const Reg scratch = Reg::X25;
      push(make_inst(MOp::LoadAddr, 8, Operand{scratch}, Operand{symbol}));
      emit_store_addr(dst, 0, Operand{scratch});
      emit_store_addr(dst, 8, Operand{Imm{static_cast<i64>(text.size())}});
      return true;
    }
    if (storage_.layout_of(value_op.type, width_).size == 0) {
      return true;
    }
    if (value_op.is<ir::FunctionIdx>()) {
      const SymbolRef sym =
          string_ref("alcy.fn." + std::to_string(value_op.as_function().idx));
      const Reg scratch = Reg::X25;
      push(make_inst(MOp::LoadAddr, 8, Operand{scratch}, Operand{sym}));
      push(make_inst(MOp::Sd, 8, Operand{scratch}, place_of(at(instr, 1))));
      return true;
    }
    const u64 store_size = storage_.layout_of(value_op.type, width_).size;
    if (store_size > 8 && value_op.is<ir::RegisterIdx>()) {
      const u32 src = vreg_of(value_op.as_register(), value_op.type);
      emit_copy(place_of(at(instr, 1)), Operand{SlotRef{src, 0}}, store_size);
      return true;
    }
    auto value = scalar_of(value_op);
    if (!value.has_value()) {
      return false;
    }
    const u8 w = store_width(value_op);
    const ir::TypeTag value_tag = storage_.types()[value_op.type.idx].tag;
    const MOp store =
        is_float_tag(value_tag) ? (w == 4 ? MOp::Fsw : MOp::Fsd) : store_of(w);
    push(make_inst(store, w, *value, place_of(at(instr, 1))));
    return true;
  }

  if (instr.op == O::ExtractValue) {
    if (at(instr, 0).is<ir::ImmutableIdx>() &&
        storage_.types()[at(instr, 0).type.idx].tag == ir::TypeTag::Str) {
      if (!at(instr, 1).is<ir::ImmutableIdx>()) {
        return false;
      }
      const ir::Immutable& imm =
          storage_.immutables()[at(instr, 0).as_immutable()];
      const std::string_view text = strings_.get(imm.data.str_id_value);
      const ir::Immutable& idx =
          storage_.immutables()[at(instr, 1).as_immutable()];
      const u64 which = idx.as_u64_integer(storage_.types()[idx.type.idx].tag);
      if (which == 0) {
        const SymbolRef symbol = string_ref(rodata_.intern(text));
        push(make_inst(MOp::LoadAddr, 8, dst_of(instr), Operand{symbol}));
      } else {
        emit_move(dst_of(instr), Operand{Imm{static_cast<i64>(text.size())}});
      }
      return true;
    }
    if (!at(instr, 0).is<ir::RegisterIdx>()) {
      return false;
    }
    const ir::TypeIdx aggregate_type = at(instr, 0).type;
    const ir::TypeTag tag = storage_.types()[aggregate_type.idx].tag;
    if (!at(instr, 1).is<ir::ImmutableIdx>()) {
      return false;
    }
    const ir::Immutable& index_imm =
        storage_.immutables()[at(instr, 1).as_immutable()];
    const u64 which =
        index_imm.as_u64_integer(storage_.types()[index_imm.type.idx].tag);
    i64 offset = 0;
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
                    storage_.struct_types()[storage_.types()[aggregate_type.idx]
                                                .as_struct()])
              : std::pair<const ir::TypeLayoutHints*, u32>{nullptr, 0};
      offset = static_cast<i64>(
          ir::field_offset(storage_.state(), fields, static_cast<u32>(which),
                           width_, hints.first, hints.second));
    } else if (tag == ir::TypeTag::Str) {
      offset = static_cast<i64>(which * 8);
    } else {
      // A two-word pair carried in one slot: each word at 8 bytes.
      offset = static_cast<i64>(which * 8);
    }
    const u32 base_vreg = vreg_of(at(instr, 0).as_register(), aggregate_type);
    if (alloca_storage_.contains(base_vreg)) {
      push(make_inst(MOp::Ld, 8, dst_of(instr),
                     Operand{SlotRef{base_vreg, static_cast<i32>(offset)}}));
    } else {
      push(make_inst(MOp::Ld, 8, dst_of(instr),
                     Operand{Mem{.base = RegRef{VReg{base_vreg}},
                                 .disp = static_cast<i32>(offset)}}));
    }
    return true;
  }

  if (instr.op == O::EnumTagSet || instr.op == O::EnumTagLoad ||
      instr.op == O::EnumFieldAddr) {
    if (!at(instr, 0).is<ir::RegisterIdx>()) {
      return false;
    }
    const ir::TypeIdx enum_type =
        ir::enum_place_type(storage_.state(), at(instr, 0).type);
    const ir::EnumNiche niche = ir::enum_niche(storage_.state(), enum_type);
    const u32 base_vreg =
        vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
    const Operand place = place_of(at(instr, 0));
    if (ir::enum_is_zst(storage_.state(), enum_type)) {
      if (instr.op == O::EnumTagLoad) {
        emit_move(dst_of(instr), Operand{ZERO});
      }
      return true;
    }
    if (instr.op == O::EnumTagSet) {
      auto tag = scalar_of(at(instr, 1));
      if (!tag.has_value()) {
        return false;
      }
      if (!niche.has) {
        const u8 tag_width = ir::enum_tag_width(storage_.state(), enum_type);
        const MOp store = tag_width == 1   ? MOp::Sb
                          : tag_width == 2 ? MOp::Sh
                          : tag_width == 4 ? MOp::Sw
                                           : MOp::Sd;
        push(make_inst(store, tag_width, *tag, place));
        return true;
      }
      const u8 width = static_cast<u8>(
          std::min<u64>(storage_.layout_of(niche.slot, width_).size, 8));
      const i64 spell =
          niche.slot == niche.payload ? 0 : static_cast<i64>(niche.sentinel);
      const u32 carrying = body_.label_count++;
      const u32 done = body_.label_count++;
      push(make_inst(MOp::Addi, 8, Operand{T1}, Operand{ZERO},
                     Operand{Imm{static_cast<i64>(niche.fieldless)}}));
      MInst branch =
          make_inst(MOp::Beq, 8, *tag, Operand{T1}, Operand{Label{carrying}});
      branch.cond = Cond::Ne;
      push(branch);
      push(make_inst(store_of(width), width, Operand{Imm{spell}}, place));
      push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
      MInst at_carrying = make_inst(MOp::Nop, 8);
      at_carrying.label = carrying;
      push(at_carrying);
      push(make_inst(store_of(width), width, Operand{ZERO}, place));
      MInst at_done = make_inst(MOp::Nop, 8);
      at_done.label = done;
      push(at_done);
      return true;
    }
    if (instr.op == O::EnumTagLoad) {
      if (!niche.has) {
        const u8 tag_width = ir::enum_tag_width(storage_.state(), enum_type);
        const MOp load = tag_width == 1   ? MOp::Lbu
                         : tag_width == 2 ? MOp::Lhu
                         : tag_width == 4 ? MOp::Lwu
                                          : MOp::Ld;
        push(make_inst(load, tag_width, dst_of(instr), place));
        return true;
      }
      const u8 width = static_cast<u8>(
          std::min<u64>(storage_.layout_of(niche.slot, width_).size, 8));
      const u32 yes = body_.label_count++;
      const u32 done = body_.label_count++;
      push(make_inst(load_of(width), width, Operand{T0}, place));
      if (niche.slot == niche.payload) {
        push(make_inst(MOp::Beq, 8, Operand{T0}, Operand{ZERO},
                       Operand{Label{yes}}));
      } else {
        push(make_inst(MOp::Addi, 8, Operand{T1}, Operand{ZERO},
                       Operand{Imm{static_cast<i64>(niche.sentinel)}}));
        MInst branch = make_inst(MOp::Bgeu, 8, Operand{T0}, Operand{T1},
                                 Operand{Label{yes}});
        branch.cond = Cond::Geu;
        push(branch);
      }
      emit_move(dst_of(instr), Operand{Imm{static_cast<i64>(niche.carrying)}});
      push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
      MInst at_yes = make_inst(MOp::Nop, 8);
      at_yes.label = yes;
      push(at_yes);
      emit_move(dst_of(instr), Operand{Imm{static_cast<i64>(niche.fieldless)}});
      MInst at_done = make_inst(MOp::Nop, 8);
      at_done.label = done;
      push(at_done);
      return true;
    }
    i64 offset = 0;
    if (!niche.has && instr.operands.size() == 3) {
      const ir::Immutable& variant_imm =
          storage_.immutables()[at(instr, 1).as_immutable()];
      const u64 variant = variant_imm.as_u64_integer(
          storage_.types()[variant_imm.type.idx].tag);
      const ir::Immutable& field_imm =
          storage_.immutables()[at(instr, 2).as_immutable()];
      const u64 field =
          field_imm.as_u64_integer(storage_.types()[field_imm.type.idx].tag);
      const ir::EnumType& shape =
          storage_.enum_types()[storage_.types()[enum_type.idx].as_enum()];
      const ir::TypeIdxRange fields =
          storage_.enum_variant_types()[shape.variants[variant]].fields;
      const ir::TypeLayout area =
          ir::enum_payload_area(storage_.state(), enum_type, width_);
      const u64 disc = ir::enum_tag_width(storage_.state(), enum_type);
      const u64 payload_start =
          area.align <= 1 ? disc
                          : (disc + area.align - 1) / area.align * area.align;
      offset = static_cast<i64>(
          payload_start + ir::field_offset(storage_.state(), fields,
                                           static_cast<u32>(field), width_));
    }
    const u32 addr_vreg =
        vreg_of(instr.dst, storage_.registers()[instr.dst].type);
    // The result names a field but holds its address.
    body_.vreg_sizes[addr_vreg] = std::max<u32>(body_.vreg_sizes[addr_vreg], 8);
    pointee_of_vreg_.insert_or_assign(addr_vreg,
                                      niche.has ? niche.payload : enum_type);
    if (alloca_storage_.contains(base_vreg)) {
      push(make_inst(MOp::Addr, 8, dst_of(instr),
                     Operand{SlotRef{base_vreg, static_cast<i32>(offset)}}));
    } else {
      push(make_inst(MOp::Addi, 8, dst_of(instr), Operand{VReg{base_vreg}},
                     Operand{Imm{offset}}));
    }
    return true;
  }

  if (instr.op == O::FAdd || instr.op == O::FSub || instr.op == O::FMul ||
      instr.op == O::FDiv) {
    if (!instr.dst.is_valid() || instr.operands.size() != 2) {
      return false;
    }
    const ir::TypeTag tag =
        storage_.types()[storage_.registers()[instr.dst].type.idx].tag;
    const u8 w = (tag == ir::TypeTag::F32) ? 4 : 8;
    auto lhs = scalar_of(at(instr, 0));
    auto rhs = scalar_of(at(instr, 1));
    if (!lhs.has_value() || !rhs.has_value()) {
      return false;
    }
    const MOp op = instr.op == O::FAdd   ? MOp::Fadd
                   : instr.op == O::FSub ? MOp::Fsub
                   : instr.op == O::FMul ? MOp::Fmul
                                         : MOp::Fdiv;
    push(make_inst(op, w, dst_of(instr), *lhs, *rhs));
    return true;
  }

  if (instr.op == O::FNeg) {
    if (!instr.dst.is_valid()) {
      return false;
    }
    auto value = scalar_of(at(instr, 0));
    if (!value.has_value()) {
      return false;
    }
    const ir::TypeTag tag =
        storage_.types()[storage_.registers()[instr.dst].type.idx].tag;
    const u8 w = (tag == ir::TypeTag::F32) ? 4 : 8;
    push(make_inst(MOp::Fneg, w, dst_of(instr), *value));
    return true;
  }

  if (instr.op == O::TypeCast) {
    auto value = scalar_of(at(instr, 0));
    if (!value.has_value()) {
      return false;
    }
    const ir::TypeTag src_tag = storage_.types()[at(instr, 0).type.idx].tag;
    const ir::TypeTag dst_tag =
        storage_.types()[storage_.registers()[instr.dst].type.idx].tag;
    const bool src_float =
        src_tag == ir::TypeTag::F32 || src_tag == ir::TypeTag::F64;
    const bool dst_float =
        dst_tag == ir::TypeTag::F32 || dst_tag == ir::TypeTag::F64;
    if (src_float && !dst_float) {
      // A float-to-integer cast saturates to the destination's range and
      // maps NaN to zero. Convert to a 64-bit integer first, then clamp.
      const u8 w = src_tag == ir::TypeTag::F32 ? 4 : 8;
      const bool uns = ir::is_unsigned_integer_type(dst_tag);
      const i64 mode = (src_tag == ir::TypeTag::F32 ? 1 : 0) + (uns ? 4 : 0);
      const u32 bits =
          static_cast<u32>(
              storage_.layout_of(storage_.registers()[instr.dst].type, width_)
                  .size) *
          8;
      constexpr Reg integer = Reg::X18;  // outside the allocator pool
      constexpr Reg scratch = Reg::X19;
      constexpr Reg edge = Reg::X20;
      const u32 not_nan = body_.label_count++;
      const u32 done = body_.label_count++;
      push(make_inst(MOp::Feq, w, Operand{scratch}, *value, *value));
      MInst nan_branch = make_inst(MOp::Bne, 8, Operand{scratch}, Operand{ZERO},
                                   Operand{Label{not_nan}});
      nan_branch.cond = Cond::Ne;
      push(nan_branch);
      push(make_inst(MOp::Addi, 8, Operand{integer}, Operand{ZERO},
                     Operand{Imm{0}}));
      push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
      MInst at_convert = make_inst(MOp::Nop, 8);
      at_convert.label = not_nan;
      push(at_convert);
      push(make_inst(MOp::Fcvt, w, Operand{integer}, *value,
                     Operand{Imm{mode}}));
      if (bits < 64) {
        // edge = 1 << (bits-1); max = edge-1 for signed, 2*edge-1 unsigned.
        push(make_inst(MOp::Addi, 8, Operand{scratch}, Operand{ZERO},
                       Operand{Imm{1}}));
        push(make_inst(MOp::Slli, 8, Operand{scratch}, Operand{scratch},
                       Operand{Imm{static_cast<i64>(bits - 1)}}));
        push(make_inst(MOp::Addi, 8, Operand{edge}, Operand{scratch},
                       Operand{Imm{-1}}));
        if (uns) {
          // max_u = 2*edge - 1; clamp unsigned values above it.
          push(make_inst(MOp::Add, 8, Operand{edge}, Operand{scratch},
                         Operand{edge}));
        }
        const u32 keep_high = body_.label_count++;
        const u32 high_done = body_.label_count++;
        MInst high = make_inst(uns ? MOp::Bltu : MOp::Blt, 8, Operand{integer},
                               Operand{edge}, Operand{Label{keep_high}});
        high.cond = uns ? Cond::Ltu : Cond::Lt;
        push(high);
        push(make_inst(MOp::Addi, 8, Operand{integer}, Operand{edge},
                       Operand{Imm{0}}));
        push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{high_done}}));
        MInst at_high = make_inst(MOp::Nop, 8);
        at_high.label = keep_high;
        push(at_high);
        MInst at_high_done = make_inst(MOp::Nop, 8);
        at_high_done.label = high_done;
        push(at_high_done);
        if (!uns) {
          // min_s = -(1 << (bits-1)); clamp signed values below it.
          const u32 keep_low = body_.label_count++;
          const u32 low_done = body_.label_count++;
          push(make_inst(MOp::Sub, 8, Operand{edge}, Operand{ZERO},
                         Operand{scratch}));
          MInst low = make_inst(MOp::Bge, 8, Operand{integer}, Operand{edge},
                                Operand{Label{keep_low}});
          low.cond = Cond::Ge;
          push(low);
          push(make_inst(MOp::Addi, 8, Operand{integer}, Operand{edge},
                         Operand{Imm{0}}));
          push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{low_done}}));
          MInst at_low = make_inst(MOp::Nop, 8);
          at_low.label = keep_low;
          push(at_low);
          MInst at_low_done = make_inst(MOp::Nop, 8);
          at_low_done.label = low_done;
          push(at_low_done);
        }
      }
      MInst at_done = make_inst(MOp::Nop, 8);
      at_done.label = done;
      push(at_done);
      emit_move(dst_of(instr), Operand{integer});
      return true;
    }
    if (src_float && dst_float && src_tag != dst_tag) {
      // Widen or narrow between f32 and f64.
      const i64 mode = src_tag == ir::TypeTag::F64 ? 6 : 7;
      push(make_inst(MOp::Fcvt, dst_tag == ir::TypeTag::F32 ? 4 : 8,
                     dst_of(instr), *value, Operand{Imm{mode}}));
      return true;
    }
    if (!src_float && dst_float) {
      // Integer -> float: pick signed/unsigned and source width.
      const bool uns = ir::is_unsigned_integer_type(src_tag);
      const u64 bits = storage_.layout_of(at(instr, 0).type, width_).size * 8;
      const bool wide = bits > 32;
      const i64 mode = dst_tag == ir::TypeTag::F32
                           ? (uns ? (wide ? 10 : 11) : (wide ? 12 : 13))
                           : (uns ? (wide ? 8 : 9) : (wide ? 2 : 3));
      const u8 w = dst_tag == ir::TypeTag::F32 ? 4 : 8;
      const Reg integer = T0;
      emit_move(Operand{integer}, *value);
      push(make_inst(MOp::Fcvt, w, dst_of(instr), Operand{integer},
                     Operand{Imm{mode}}));
      return true;
    }
    // Pointers and integers of the same width are the same bits; narrow
    // sources are already zero/sign-extended into their slot by the
    // store that placed them there.
    emit_move(dst_of(instr), *value);
    return true;
  }

  if (instr.op == O::GetElementPtr) {
    if (!at(instr, 0).is<ir::RegisterIdx>()) {
      return false;
    }
    const u32 base_vreg =
        vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
    ir::TypeIdx type =
        pointee_of_vreg_.contains(base_vreg)
            ? pointee_of_vreg_.at(base_vreg)
            : storage_.registers()[at(instr, 0).as_register()].type;
    const u32 count = instr.operands.size();
    const bool dynamic =
        count > 2 && at(instr, count - 1).is<ir::RegisterIdx>();
    const i64 offset =
        projection_offset(instr, dynamic ? count - 1 : count, type);
    const u32 addr_vreg =
        vreg_of(instr.dst, storage_.registers()[instr.dst].type);
    // The register names an element, but holds its address: keep the
    // element's type yet give the slot a full pointer's width.
    body_.vreg_sizes[addr_vreg] = std::max<u32>(body_.vreg_sizes[addr_vreg], 8);
    pointee_of_vreg_.insert_or_assign(addr_vreg, type);
    const Operand addr = Operand{VReg{addr_vreg}};
    if (alloca_storage_.contains(base_vreg)) {
      push(make_inst(MOp::Addr, 8, addr,
                     Operand{SlotRef{base_vreg, static_cast<i32>(offset)}}));
    } else {
      // The allocator resolves the base vreg into a scratch register.
      push(make_inst(MOp::Addi, 8, addr, Operand{VReg{base_vreg}},
                     Operand{Imm{offset}}));
    }
    if (dynamic) {
      // A register index scales by the element size; projection_offset
      // cannot encode it as a byte displacement.
      u64 stride = 1;
      const ir::TypeTag tag = storage_.types()[type.idx].tag;
      if (tag == ir::TypeTag::Array) {
        const ir::ArrayType& array =
            storage_.array_types()[storage_.types()[type.idx].as_array()];
        stride = storage_.layout_of(array.element, width_).size;
      } else if (passage_of(type) == Passage::TwoWord) {
        stride = 8;
      } else {
        stride = storage_.layout_of(type, width_).size;
      }
      auto index = scalar_of(at(instr, count - 1));
      if (!index.has_value()) {
        return false;
      }
      constexpr Reg scaled = Reg::X26;
      constexpr Reg result = Reg::X27;
      emit_move(Operand{scaled}, *index);
      push(make_inst(MOp::Addi, 8, Operand{result}, Operand{ZERO},
                     Operand{Imm{static_cast<i64>(stride)}}));
      push(make_inst(MOp::Mul, 8, Operand{result}, Operand{scaled},
                     Operand{result}));
      push(make_inst(MOp::Add, 8, addr, addr, Operand{result}));
    }
    return true;
  }

  if (instr.op == O::Select) {
    // operands = [cond, true_val, false_val].
    if (instr.operands.size() != 3) {
      return false;
    }
    auto cond = scalar_of(at(instr, 0));
    auto yes = scalar_of(at(instr, 1));
    auto no = scalar_of(at(instr, 2));
    if (!cond.has_value() || !yes.has_value() || !no.has_value()) {
      return false;
    }
    const u32 taken = body_.label_count++;
    const u32 done = body_.label_count++;
    MInst branch =
        make_inst(MOp::Bne, 8, *cond, Operand{ZERO}, Operand{Label{taken}});
    branch.cond = Cond::Ne;
    push(branch);
    emit_move(dst_of(instr), *no);
    push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{done}}));
    MInst at_taken = make_inst(MOp::Nop, 8);
    at_taken.label = taken;
    push(at_taken);
    emit_move(dst_of(instr), *yes);
    MInst at_done = make_inst(MOp::Nop, 8);
    at_done.label = done;
    push(at_done);
    return true;
  }

  if (instr.op == O::Memcopy) {
    // operands = [dst, src, len]. Copy bytes with a loop.
    auto dst = scalar_of(at(instr, 0));
    auto src = scalar_of(at(instr, 1));
    auto len = scalar_of(at(instr, 2));
    if (!dst.has_value() || !src.has_value() || !len.has_value()) {
      return false;
    }
    // The loop counters live across instructions, so they must avoid
    // the allocator's per-instruction scratch pool (t0-t2, ...).
    constexpr Reg dst_reg = Reg::X25;
    constexpr Reg src_reg = Reg::X26;
    constexpr Reg len_reg = Reg::X27;
    emit_move(Operand{dst_reg}, *dst);
    emit_move(Operand{src_reg}, *src);
    emit_move(Operand{len_reg}, *len);
    const u32 loop = body_.label_count++;
    const u32 done = body_.label_count++;
    MInst at_loop = make_inst(MOp::Nop, 8);
    at_loop.label = loop;
    push(at_loop);
    push(make_inst(MOp::Beq, 8, Operand{len_reg}, Operand{ZERO},
                   Operand{Label{done}}));
    push(make_inst(MOp::Lb, 1, Operand{A0},
                   Operand{Mem{.base = RegRef{src_reg}, .disp = 0}}));
    push(make_inst(MOp::Sb, 1, Operand{A0},
                   Operand{Mem{.base = RegRef{dst_reg}, .disp = 0}}));
    push(make_inst(MOp::Addi, 8, Operand{dst_reg}, Operand{dst_reg},
                   Operand{Imm{1}}));
    push(make_inst(MOp::Addi, 8, Operand{src_reg}, Operand{src_reg},
                   Operand{Imm{1}}));
    push(make_inst(MOp::Addi, 8, Operand{len_reg}, Operand{len_reg},
                   Operand{Imm{-1}}));
    push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{loop}}));
    MInst at_done = make_inst(MOp::Nop, 8);
    at_done.label = done;
    push(at_done);
    return true;
  }

  if (instr.op == O::Memset) {
    // operands = [dst, value, len]. Fill bytes with a simple loop:
    //   x25=cursor, x26=remaining, x27=value.
    auto dst = scalar_of(at(instr, 0));
    auto value = scalar_of(at(instr, 1));
    auto len = scalar_of(at(instr, 2));
    if (!dst.has_value() || !value.has_value() || !len.has_value()) {
      return false;
    }
    // Loop state lives across instructions, so keep it outside the
    // allocator's scratch pool; a large length immediate is materialized
    // into pool[0] and would clobber the cursor otherwise.
    constexpr Reg cursor = Reg::X25;
    constexpr Reg remaining = Reg::X26;
    constexpr Reg byte_value = Reg::X27;
    emit_move(Operand{cursor}, *dst);
    emit_move(Operand{remaining}, *len);
    emit_move(Operand{byte_value}, *value);
    const u32 loop = body_.label_count++;
    const u32 done = body_.label_count++;
    MInst at_loop = make_inst(MOp::Nop, 8);
    at_loop.label = loop;
    push(at_loop);
    push(make_inst(MOp::Beq, 8, Operand{remaining}, Operand{ZERO},
                   Operand{Label{done}}));
    push(make_inst(MOp::Sb, 1, Operand{byte_value},
                   Operand{Mem{.base = RegRef{cursor}, .disp = 0}}));
    push(make_inst(MOp::Addi, 8, Operand{cursor}, Operand{cursor},
                   Operand{Imm{1}}));
    push(make_inst(MOp::Addi, 8, Operand{remaining}, Operand{remaining},
                   Operand{Imm{-1}}));
    push(make_inst(MOp::Jal, 8, Operand{ZERO}, Operand{Label{loop}}));
    MInst at_done = make_inst(MOp::Nop, 8);
    at_done.label = done;
    push(at_done);
    return true;
  }

  if (instr.op == O::ElemOffset) {
    // operands = [base_ptr, index]; the destination's type names the
    // element, so the byte stride comes from its pointee.
    if (!at(instr, 0).is<ir::RegisterIdx>() || instr.operands.size() != 2) {
      return false;
    }
    const ir::TypeIdx dst_type = storage_.registers()[instr.dst].type;
    const ir::TypeTag dst_tag = storage_.types()[dst_type.idx].tag;
    ir::TypeIdx element = dst_type;
    if (dst_tag == ir::TypeTag::Ref || dst_tag == ir::TypeTag::MutRef ||
        dst_tag == ir::TypeTag::Ptr || dst_tag == ir::TypeTag::RawPtr ||
        dst_tag == ir::TypeTag::RawMutPtr) {
      element =
          storage_.ref_types()[storage_.types()[dst_type.idx].as_ref()].pointee;
    }
    const u64 stride = storage_.layout_of(element, width_).size;
    auto index = scalar_of(at(instr, 1));
    if (!index.has_value()) {
      return false;
    }
    const u32 base_vreg =
        vreg_of(at(instr, 0).as_register(), at(instr, 0).type);
    const u32 addr_vreg = vreg_of(instr.dst, dst_type);
    // The destination names the element reached but holds its address;
    // keep the type yet give the slot a full pointer's width.
    body_.vreg_sizes[addr_vreg] = std::max<u32>(body_.vreg_sizes[addr_vreg], 8);
    if (const Imm* imm = std::get_if<Imm>(&*index); imm != nullptr) {
      const i64 disp = imm->value * static_cast<i64>(stride);
      if (alloca_storage_.contains(base_vreg)) {
        push(make_inst(MOp::Addr, 8, dst_of(instr),
                       Operand{SlotRef{base_vreg, static_cast<i32>(disp)}}));
      } else {
        push(make_inst(MOp::Addi, 8, dst_of(instr), Operand{VReg{base_vreg}},
                       Operand{Imm{disp}}));
      }
    } else {
      // A register index: materialize the stride into a scratch, scale
      // the index, then add it to the base.
      // Use scratch registers outside the allocator pool: the final Add's
      // destination vreg is materialized into pool[0] (T0), which would
      // clobber the scaled index held there.
      constexpr Reg idx = Reg::X25;
      constexpr Reg scaled = Reg::X26;
      emit_move(Operand{idx}, *index);
      push(make_inst(MOp::Addi, 8, Operand{scaled}, Operand{ZERO},
                     Operand{Imm{static_cast<i64>(stride)}}));
      push(make_inst(MOp::Mul, 8, Operand{scaled}, Operand{idx},
                     Operand{scaled}));
      push(make_inst(MOp::Add, 8, dst_of(instr), Operand{VReg{base_vreg}},
                     Operand{scaled}));
    }
    pointee_of_vreg_.insert_or_assign(vreg_of(instr.dst, dst_type), element);
    return true;
  }

  if (instr.op == O::Call) {
    return emit_call(instr, span);
  }

  return unsupported(span, ir::opcode_to_str(instr.op));
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

bool Selector::unsupported(const diag::Span span, const std::string_view what) {
  if (!failed_) {
    (void)bag_.emit<i18n::Key::CodegenRiscv64Unsupported>(
        DiagCode::Unsupported, span, std::string(what));
    failed_ = true;
  }
  return false;
}

bool Selector::emit_call(const ir::Instruction& instr, const diag::Span span) {
  u32 next_arg = 0;
  u32 next_float = 0;
  const bool aggregate_result =
      instr.dst.is_valid() &&
      passage_of(storage_.registers()[instr.dst].type) == Passage::ByAddress;
  if (aggregate_result) {
    const u32 vreg = vreg_of(instr.dst, storage_.registers()[instr.dst].type);
    alloca_storage_.insert(vreg);
    push(make_inst(MOp::Addr, 8, Operand{ARG_REGS[next_arg++]},
                   Operand{SlotRef{vreg, 0}}));
  }
  const ir::Operand& callee_op = at(instr, 0);
  const bool indirect = callee_op.is<ir::RegisterIdx>();
  u32 closure_vreg = 0;
  if (indirect) {
    closure_vreg = vreg_of(callee_op.as_register(), callee_op.type);
    push(make_inst(MOp::Ld, 8, Operand{ARG_REGS[next_arg++]},
                   Operand{SlotRef{closure_vreg, 8}}));
  }
  for (u32 at_index = 1; at_index < instr.operands.size(); ++at_index) {
    const ir::Operand& arg = at(instr, at_index);
    const ir::TypeTag tag = storage_.types()[arg.type.idx].tag;
    if (is_float_tag(tag)) {
      if (next_float >= ARG_FREG_COUNT) {
        return unsupported(
            span, "a call with more than eight floating-point arguments");
      }
      const u8 w = tag == ir::TypeTag::F32 ? 4 : 8;
      const u32 vreg =
          arg.is<ir::RegisterIdx>() ? vreg_of(arg.as_register(), arg.type) : 0;
      if (arg.is<ir::RegisterIdx>()) {
        push(make_inst(w == 4 ? MOp::Flw : MOp::Fld, w,
                       Operand{ARG_FREGS[next_float++]},
                       Operand{SlotRef{vreg, 0}}));
      } else {
        auto side = scalar_of(arg);
        if (!side.has_value()) {
          return unsupported(span,
                             "a floating-point call argument with no lowering");
        }
        push(make_inst(MOp::Fmov, w, Operand{ARG_FREGS[next_float++]}, *side));
      }
      continue;
    }
    if (passage_of(arg.type) == Passage::ByAddress) {
      if (!arg.is<ir::RegisterIdx>() || next_arg >= ARG_REG_COUNT) {
        return unsupported(
            span, "a call argument passed by address that is not a register");
      }
      push(
          make_inst(MOp::Addr, 8, Operand{ARG_REGS[next_arg++]},
                    Operand{SlotRef{vreg_of(arg.as_register(), arg.type), 0}}));
      continue;
    }
    if (passage_of(arg.type) == Passage::TwoWord) {
      if (next_arg + 1 >= ARG_REG_COUNT) {
        return unsupported(span,
                           "a call with more than eight argument registers");
      }
      if (arg.is<ir::ImmutableIdx>() && tag == ir::TypeTag::Str) {
        const ir::Immutable& imm = storage_.immutables()[arg.as_immutable()];
        const std::string_view text = strings_.get(imm.data.str_id_value);
        const SymbolRef symbol = string_ref(rodata_.intern(text));
        push(make_inst(MOp::LoadAddr, 8, Operand{ARG_REGS[next_arg++]},
                       Operand{symbol}));
        emit_move(Operand{ARG_REGS[next_arg++]},
                  Operand{Imm{static_cast<i64>(text.size())}});
      } else if (arg.is<ir::RegisterIdx>()) {
        const u32 vreg = vreg_of(arg.as_register(), arg.type);
        push(make_inst(MOp::Ld, 8, Operand{ARG_REGS[next_arg++]},
                       Operand{SlotRef{vreg, 0}}));
        push(make_inst(MOp::Ld, 8, Operand{ARG_REGS[next_arg++]},
                       Operand{SlotRef{vreg, 8}}));
      } else {
        return unsupported(span, "a two-word call argument with no lowering");
      }
      continue;
    }
    if (next_arg >= ARG_REG_COUNT) {
      return unsupported(span, "a call with more than eight integer arguments");
    }
    auto value = scalar_of(arg);
    if (!value.has_value()) {
      return unsupported(span, "a call argument with no lowering");
    }
    emit_move(Operand{ARG_REGS[next_arg++]}, *value);
  }
  const ir::Operand& callee = callee_op;
  std::string symbol;
  if (callee.is<ir::FunctionIdx>()) {
    symbol = "alcy.fn." + std::to_string(callee.as_function().idx);
  } else if (callee.is<ir::ParametricCalleeIdx>()) {
    // A parametric callee carries the instantiated function it resolved
    // to; that is the body to call.
    const ir::ParametricCallee& pcallee =
        storage_.parametric_callees()[callee.as_parametric_callee()];
    if (!pcallee.function.is_valid()) {
      return unsupported(span, "a parametric callee with no concrete function");
    }
    symbol = "alcy.fn." + std::to_string(pcallee.function.idx);
  } else if (callee.is<ir::ExternalFunctionIdx>()) {
    symbol = std::string(strings_.get(
        storage_.external_functions()[callee.as_external_function()]
            .meta.name));
  } else if (!indirect) {
    return unsupported(span, "a callee with no spelling");
  }
  const bool sited =
      backtrace_ && current_instr_idx_.idx < site_names_.size() &&
      site_names_[current_instr_idx_.idx] != str::EMPTY_STRING_ID;
  const u32 call_label = sited ? body_.label_count++ : MInst::NO_LABEL;
  const u32 symbol_id = indirect ? MInst::NO_LABEL : string_ref(symbol).id;
  if (indirect) {
    const Reg target = Reg::X27;
    push(make_inst(MOp::Ld, 8, Operand{target},
                   Operand{SlotRef{closure_vreg, 0}}));
    MInst call =
        make_inst(MOp::Jalr, 8, Operand{RA}, Operand{target}, Operand{Imm{0}});
    call.label = call_label;
    push(call);
  } else {
    // Resolve a direct call through a full-width address pair. The
    // backend patches the auipc/addi once offsets are known, so the call
    // never needs an instruction inserted after layout was fixed.
    push(make_inst(MOp::Auipc, 8, Operand{T0}, Operand{Imm{0}}));
    push(make_inst(MOp::Addi, 8, Operand{T0}, Operand{T0},
                   Operand{SymbolRef{symbol_id}}));
    MInst call =
        make_inst(MOp::Jalr, 8, Operand{RA}, Operand{T0}, Operand{Imm{0}});
    call.label = call_label;
    push(call);
  }
  if (sited) {
    body_.call_sites.push_back(Body::CallSite{
        call_label,
        std::string(strings_.get(site_names_[current_instr_idx_.idx]))});
  }
  if (instr.dst.is_valid() && !aggregate_result) {
    const ir::TypeIdx dst_type = storage_.registers()[instr.dst].type;
    const ir::TypeTag tag = storage_.types()[dst_type.idx].tag;
    const u64 size = storage_.layout_of(dst_type, width_).size;
    const u32 vreg = vreg_of(instr.dst, dst_type);
    if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
      push(make_inst(tag == ir::TypeTag::F32 ? MOp::Fsw : MOp::Fsd,
                     tag == ir::TypeTag::F32 ? 4 : 8, Operand{Reg::F10},
                     Operand{SlotRef{vreg, 0}}));
    } else if (size <= 8) {
      push(make_inst(MOp::Sd, 8, Operand{A0}, Operand{SlotRef{vreg, 0}}));
    } else {
      // A multiword result lives in this vreg's slot; mark it so a
      // later projection reads the words instead of treating the first
      // one as a pointer to the value.
      alloca_storage_.insert(vreg);
      push(make_inst(MOp::Sd, 8, Operand{A0}, Operand{SlotRef{vreg, 0}}));
      push(make_inst(MOp::Sd, 8, Operand{A1}, Operand{SlotRef{vreg, 8}}));
    }
  }
  return true;
}

}  // namespace

base::Result<Body, EmitError> select_function(const SelectContext& context) {
  Selector selector(context);
  return selector.run();
}

}  // namespace codegen::riscv64
