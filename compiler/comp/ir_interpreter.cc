// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "comp/ir_interpreter.h"

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "comp/comp_value.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace comp {
namespace {

using ir::Opcode;
using ir::TypeTag;

// The run's memory ceiling. The quota bounds iterations, not bytes, so
// a single large allocation needs a bound of its own.
constexpr u64 MEMORY_LIMIT = 1ull << 28;

// A register's value: scalar bits, or the address of a memory object.
struct Slot {
  enum class Kind : u8 { Scalar, Address };
  Kind kind = Kind::Scalar;
  u64 bits = 0;
  u64 address = 0;
};

u32 int_width_bits(TypeTag tag) {
  return tag == TypeTag::I1 ? 1 : int_bytes(tag) * 8;
}

// The width bits an integer of `tag` keeps. `comp::mask` treats I1 as
// a default-width integer; the engine must not.
u64 int_mask(TypeTag tag) {
  return tag == TypeTag::I1 ? 1 : mask(tag);
}

class Interpreter {
 public:
  Interpreter(const ir::StorageState& storage,
              const ir::SymbolTable& strings,
              ir::PointerWidth width,
              u64& quota,
              u32& depth)
      : storage_(storage),
        strings_(strings),
        width_(width),
        quota_(quota),
        depth_(depth) {
    // Address zero is never a live object.
    memory_.push_back(0);
  }

  base::Result<CompVal, IrRunError> run(ir::FunctionIdx function,
                                        std::span<const CompVal> args);

 private:
  struct Frame {
    std::vector<Slot> regs;
  };

  struct Returned {
    Slot slot;
    ir::TypeIdx type = ir::TypeIdx(base::INVALID_IDX);
  };

  const ir::Operand& operand_at(ir::InstructionIdx instr, u32 offset) const {
    const ir::Instruction& ins = storage_.instrs[instr];
    return storage_.operands[ir::OperandIdx(ins.operands.head().idx + offset)];
  }

  TypeTag tag_of(ir::TypeIdx type) const { return storage_.types[type].tag; }

  ir::TypeIdx reg_type(ir::RegisterIdx reg) const {
    return storage_.registers[reg].type;
  }

  ir::TypeLayout layout_of(ir::TypeIdx type) const {
    return ir::type_layout(storage_, type, width_);
  }

  u64 word_bytes() const { return width_ == ir::PointerWidth::W64 ? 8 : 4; }

  ir::TypeIdx pointee_of(ir::TypeIdx type) const {
    return storage_.ref_types[storage_.types[type].as_ref()].pointee;
  }

  bool fail(IrRunError::Kind kind,
            ir::InstructionIdx instr,
            std::string_view what) {
    if (!error_.has_value()) {
      error_ = IrRunError{kind, instr, what};
    }
    return false;
  }

  base::Result<CompVal, IrRunError> error_result() {
    if (!error_.has_value()) {
      return base::make_err(IrRunError{IrRunError::Kind::Internal,
                                       ir::InstructionIdx(base::INVALID_IDX),
                                       "interpreter failed without an error"});
    }
    const IrRunError error = *error_;
    error_.reset();
    return base::make_err(error);
  }

  bool spend(ir::InstructionIdx instr) {
    if (quota_ == 0) {
      return fail(IrRunError::Kind::Quota, instr,
                  "comp evaluation quota exhausted");
    }
    --quota_;
    return true;
  }

  bool alloc(ir::TypeIdx type,
             u64 count,
             ir::InstructionIdx instr,
             u64& address);
  bool read(ir::TypeIdx type, u64 address, ir::InstructionIdx instr, Slot& out);
  bool write(ir::TypeIdx type,
             const Slot& value,
             u64 address,
             ir::InstructionIdx instr);
  bool read_bytes(u64 address, u64 size, ir::InstructionIdx instr, u64& out);
  bool write_bytes(u64 address, u64 size, u64 value, ir::InstructionIdx instr);

  bool resolve(const ir::Operand& operand,
               ir::InstructionIdx instr,
               const Frame& frame,
               Slot& out);
  bool resolve_at(ir::InstructionIdx instr,
                  u32 offset,
                  const Frame& frame,
                  Slot& out);
  bool resolve_int_at(ir::InstructionIdx instr,
                      u32 offset,
                      const Frame& frame,
                      u64& out);

  bool bind_slot(ir::TypeIdx type,
                 const Slot& from,
                 Slot& to,
                 ir::InstructionIdx instr);
  bool comp_val_to_slot(ir::TypeIdx type, const CompVal& value, Slot& out);

  bool execute_function(ir::FunctionIdx function,
                        std::span<const Slot> args,
                        ir::InstructionIdx at,
                        Returned& out);
  bool execute_blocks(const ir::Function& function,
                      Frame& frame,
                      Returned& out);
  bool execute_instruction(ir::InstructionIdx instr,
                           ir::BlockIdx block,
                           Frame& frame);
  bool execute_gep(ir::InstructionIdx instr, Frame& frame);
  bool execute_binary(ir::InstructionIdx instr, Frame& frame);
  bool execute_not(ir::InstructionIdx instr, Frame& frame);
  bool execute_bit_reverse(ir::InstructionIdx instr, Frame& frame);
  bool execute_compare(ir::InstructionIdx instr, Frame& frame);
  bool execute_cast(ir::InstructionIdx instr, Frame& frame);
  bool execute_select(ir::InstructionIdx instr, Frame& frame);
  bool execute_call(ir::InstructionIdx instr, Frame& frame);

  base::Result<CompVal, IrRunError> to_result(const Returned& returned);

  const ir::StorageState& storage_;
  const ir::SymbolTable& strings_;
  ir::PointerWidth width_;
  u64& quota_;
  u32& depth_;
  // The run's objects in one arena. An address stays valid for the whole
  // run, so a reference passed down a call reads the caller's object.
  std::vector<u8> memory_;
  std::optional<IrRunError> error_;
  bool jumped_ = false;
  ir::BlockIdx jump_target_ = ir::BlockIdx(base::INVALID_IDX);
  bool returned_ = false;
  Returned returned_value_;
};

bool Interpreter::alloc(ir::TypeIdx type,
                        u64 count,
                        ir::InstructionIdx instr,
                        u64& address) {
  const ir::TypeLayout layout = layout_of(type);
  const u64 align = layout.align == 0 ? 1 : layout.align;
  if (count != 0 && layout.size > MEMORY_LIMIT / count) {
    return fail(IrRunError::Kind::Quota, instr,
                "comp evaluation memory exhausted");
  }
  const u64 bytes = layout.size * count;
  const u64 base = ir::align_up(static_cast<u64>(memory_.size()), align);
  if (base > MEMORY_LIMIT || bytes > MEMORY_LIMIT - base) {
    return fail(IrRunError::Kind::Quota, instr,
                "comp evaluation memory exhausted");
  }
  memory_.resize(static_cast<usize>(base + bytes), 0);
  address = base;
  return true;
}

bool Interpreter::read_bytes(u64 address,
                             u64 size,
                             ir::InstructionIdx instr,
                             u64& out) {
  const u64 size_available = static_cast<u64>(memory_.size());
  if (address > size_available || size > size_available - address) {
    return fail(IrRunError::Kind::Internal, instr,
                "memory access out of bounds");
  }
  u64 value = 0;
  for (u64 i = 0; i < size; ++i) {
    value |= static_cast<u64>(memory_[static_cast<usize>(address + i)])
             << (8 * i);
  }
  out = value;
  return true;
}

bool Interpreter::write_bytes(u64 address,
                              u64 size,
                              u64 value,
                              ir::InstructionIdx instr) {
  const u64 size_available = static_cast<u64>(memory_.size());
  if (address > size_available || size > size_available - address) {
    return fail(IrRunError::Kind::Internal, instr,
                "memory access out of bounds");
  }
  for (u64 i = 0; i < size; ++i) {
    memory_[static_cast<usize>(address + i)] =
        static_cast<u8>(value >> (8 * i));
  }
  return true;
}

bool Interpreter::read(ir::TypeIdx type,
                       u64 address,
                       ir::InstructionIdx instr,
                       Slot& out) {
  const TypeTag tag = tag_of(type);
  if (ir::is_integer_type(tag)) {
    u64 bits = 0;
    if (!read_bytes(address, layout_of(type).size, instr, bits)) {
      return false;
    }
    out = Slot{Slot::Kind::Scalar, bits & int_mask(tag), 0};
    return true;
  }
  if (ir::is_pointer_type(tag)) {
    if (layout_of(type).size != word_bytes()) {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "fat reference load in comp evaluation");
    }
    u64 word = 0;
    if (!read_bytes(address, word_bytes(), instr, word)) {
      return false;
    }
    out = Slot{Slot::Kind::Address, 0, word};
    return true;
  }
  if (ir::is_float_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "float load in comp evaluation");
  }
  return fail(IrRunError::Kind::Unsupported, instr,
              "aggregate load in comp evaluation");
}

bool Interpreter::write(ir::TypeIdx type,
                        const Slot& value,
                        u64 address,
                        ir::InstructionIdx instr) {
  const TypeTag tag = tag_of(type);
  if (ir::is_integer_type(tag)) {
    if (value.kind != Slot::Kind::Scalar) {
      return fail(IrRunError::Kind::Internal, instr,
                  "integer store of a non-scalar");
    }
    return write_bytes(address, layout_of(type).size,
                       value.bits & int_mask(tag), instr);
  }
  if (ir::is_pointer_type(tag)) {
    if (layout_of(type).size != word_bytes()) {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "fat reference store in comp evaluation");
    }
    if (value.kind != Slot::Kind::Address) {
      return fail(IrRunError::Kind::Internal, instr,
                  "reference store of a non-address");
    }
    return write_bytes(address, word_bytes(), value.address, instr);
  }
  if (ir::is_float_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "float store in comp evaluation");
  }
  return fail(IrRunError::Kind::Unsupported, instr,
              "aggregate store in comp evaluation");
}

bool Interpreter::resolve(const ir::Operand& operand,
                          ir::InstructionIdx instr,
                          const Frame& frame,
                          Slot& out) {
  if (operand.is<ir::RegisterIdx>()) {
    out = frame.regs[operand.as_register().idx];
    return true;
  }
  if (operand.is<ir::ImmutableIdx>()) {
    const ir::Immutable& immutable =
        storage_.immutables[operand.as_immutable()];
    const TypeTag tag = tag_of(immutable.type);
    if (!ir::is_integer_type(tag)) {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "non-integer constant in comp evaluation");
    }
    out = Slot{Slot::Kind::Scalar,
               immutable.as_u64_integer(tag) & int_mask(tag), 0};
    return true;
  }
  return fail(IrRunError::Kind::Internal, instr, "operand without a value");
}

bool Interpreter::resolve_at(ir::InstructionIdx instr,
                             u32 offset,
                             const Frame& frame,
                             Slot& out) {
  if (offset >= storage_.instrs[instr].operands.size()) {
    return fail(IrRunError::Kind::Internal, instr,
                "instruction is missing an operand");
  }
  return resolve(operand_at(instr, offset), instr, frame, out);
}

bool Interpreter::resolve_int_at(ir::InstructionIdx instr,
                                 u32 offset,
                                 const Frame& frame,
                                 u64& out) {
  Slot slot;
  if (!resolve_at(instr, offset, frame, slot)) {
    return false;
  }
  if (slot.kind != Slot::Kind::Scalar) {
    return fail(IrRunError::Kind::Internal, instr,
                "integer operand is not a scalar");
  }
  out = slot.bits & int_mask(tag_of(operand_at(instr, offset).type));
  return true;
}

bool Interpreter::bind_slot(ir::TypeIdx type,
                            const Slot& from,
                            Slot& to,
                            ir::InstructionIdx instr) {
  const TypeTag tag = tag_of(type);
  if (tag == TypeTag::Void) {
    to = Slot{};
    return true;
  }
  if (ir::is_integer_type(tag)) {
    if (from.kind != Slot::Kind::Scalar) {
      return fail(IrRunError::Kind::Internal, instr,
                  "integer value of a non-scalar");
    }
    to = Slot{Slot::Kind::Scalar, from.bits & int_mask(tag), 0};
    return true;
  }
  if (ir::is_pointer_type(tag)) {
    if (from.kind != Slot::Kind::Address) {
      return fail(IrRunError::Kind::Internal, instr,
                  "reference value of a non-address");
    }
    to = from;
    return true;
  }
  if (ir::is_float_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "float value in comp evaluation");
  }
  return fail(IrRunError::Kind::Unsupported, instr,
              "aggregate value in comp evaluation");
}

bool Interpreter::comp_val_to_slot(ir::TypeIdx type,
                                   const CompVal& value,
                                   Slot& out) {
  const TypeTag tag = tag_of(type);
  if (tag == TypeTag::Void) {
    out = Slot{};
    return true;
  }
  if (ir::is_integer_type(tag)) {
    if (value.value.tag == CompValue::Tag::Bool) {
      out = Slot{Slot::Kind::Scalar,
                 static_cast<u64>(value.value.bool_value ? 1 : 0), 0};
      return true;
    }
    if (value.value.tag == CompValue::Tag::Int) {
      out = Slot{Slot::Kind::Scalar, value.value.int_value & int_mask(tag), 0};
      return true;
    }
    return fail(IrRunError::Kind::Unsupported,
                ir::InstructionIdx(base::INVALID_IDX),
                "comp argument is not an integer");
  }
  if (ir::is_pointer_type(tag)) {
    return fail(IrRunError::Kind::Unsupported,
                ir::InstructionIdx(base::INVALID_IDX),
                "reference argument in comp evaluation");
  }
  if (ir::is_float_type(tag)) {
    return fail(IrRunError::Kind::Unsupported,
                ir::InstructionIdx(base::INVALID_IDX),
                "float argument in comp evaluation");
  }
  return fail(IrRunError::Kind::Unsupported,
              ir::InstructionIdx(base::INVALID_IDX),
              "aggregate argument in comp evaluation");
}

base::Result<CompVal, IrRunError> Interpreter::run(
    ir::FunctionIdx function,
    std::span<const CompVal> args) {
  // The same incompleteness the call path guards against applies to the
  // entry the caller names.
  if (function.idx >= storage_.functions.size()) {
    fail(IrRunError::Kind::Unsupported, ir::InstructionIdx(base::INVALID_IDX),
         "function that is not lowered yet");
    return error_result();
  }
  const ir::Function& fn = storage_.functions[function];
  const ir::Block& entry = storage_.blocks[fn.blocks.head()];
  if (args.size() != entry.block_params.size()) {
    fail(IrRunError::Kind::Internal, ir::InstructionIdx(base::INVALID_IDX),
         "call arity does not match parameters");
    return error_result();
  }
  std::vector<Slot> bound;
  bound.reserve(args.size());
  u32 at = 0;
  for (const ir::BlockParamIdx param_id : entry.block_params) {
    const ir::BlockParam& param = storage_.block_params[param_id];
    Slot slot;
    if (!comp_val_to_slot(param.type, args[at], slot)) {
      return error_result();
    }
    bound.push_back(slot);
    ++at;
  }
  Returned returned;
  if (!execute_function(function, bound, ir::InstructionIdx(base::INVALID_IDX),
                        returned)) {
    return error_result();
  }
  return to_result(returned);
}

bool Interpreter::execute_function(ir::FunctionIdx function,
                                   std::span<const Slot> args,
                                   ir::InstructionIdx at,
                                   Returned& out) {
  if (!spend(at)) {
    return false;
  }
  if (depth_ >= COMP_MAX_CALL_DEPTH) {
    return fail(IrRunError::Kind::Depth, at, "comp call depth exhausted");
  }
  const ir::Function& fn = storage_.functions[function];
  const ir::Block& entry = storage_.blocks[fn.blocks.head()];
  if (args.size() != entry.block_params.size()) {
    return fail(IrRunError::Kind::Internal, at,
                "call arity does not match parameters");
  }
  Frame frame;
  frame.regs.resize(storage_.registers.size());
  u32 position = 0;
  for (const ir::BlockParamIdx param_id : entry.block_params) {
    const ir::BlockParam& param = storage_.block_params[param_id];
    Slot bound;
    if (!bind_slot(param.type, args[position], bound, at)) {
      return false;
    }
    frame.regs[param.reg.idx] = bound;
    ++position;
  }
  ++depth_;
  const bool ok = execute_blocks(fn, frame, out);
  --depth_;
  return ok;
}

bool Interpreter::execute_blocks(const ir::Function& function,
                                 Frame& frame,
                                 Returned& out) {
  ir::BlockIdx block_idx = function.blocks.head();
  while (true) {
    const ir::Block& block = storage_.blocks[block_idx];
    bool moved = false;
    for (const ir::InstructionIdx instr : block.instrs) {
      jumped_ = false;
      returned_ = false;
      if (!execute_instruction(instr, block_idx, frame)) {
        return false;
      }
      if (returned_) {
        out = returned_value_;
        return true;
      }
      if (jumped_) {
        moved = true;
        break;
      }
    }
    if (!moved) {
      return fail(IrRunError::Kind::Internal, block.instrs.head(),
                  "block without terminator");
    }
    block_idx = jump_target_;
  }
}

bool Interpreter::execute_instruction(ir::InstructionIdx instr,
                                      ir::BlockIdx block,
                                      Frame& frame) {
  const ir::Instruction& ins = storage_.instrs[instr];
  switch (ins.op) {
    case Opcode::Noop: return true;
    case Opcode::Move:
    case Opcode::Borrow: {
      if (!ins.dst.is_valid()) {
        return true;
      }
      Slot value;
      if (!resolve_at(instr, 0, frame, value)) {
        return false;
      }
      frame.regs[ins.dst.idx] = value;
      return true;
    }
    case Opcode::Drop: return true;
    case Opcode::Alloca: {
      if (!ins.dst.is_valid()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "alloca without a destination");
      }
      u64 count = 0;
      if (!resolve_int_at(instr, 0, frame, count)) {
        return false;
      }
      u64 address = 0;
      if (!alloc(reg_type(ins.dst), count, instr, address)) {
        return false;
      }
      frame.regs[ins.dst.idx] = Slot{Slot::Kind::Address, 0, address};
      return true;
    }
    case Opcode::Load: {
      if (!ins.dst.is_valid()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "load without a destination");
      }
      Slot pointer;
      if (!resolve_at(instr, 0, frame, pointer)) {
        return false;
      }
      if (pointer.kind != Slot::Kind::Address) {
        return fail(IrRunError::Kind::Internal, instr,
                    "load from a non-address");
      }
      Slot value;
      if (!read(reg_type(ins.dst), pointer.address, instr, value)) {
        return false;
      }
      frame.regs[ins.dst.idx] = value;
      return true;
    }
    case Opcode::Store: {
      Slot value;
      Slot pointer;
      if (!resolve_at(instr, 0, frame, value)) {
        return false;
      }
      if (!resolve_at(instr, 1, frame, pointer)) {
        return false;
      }
      if (pointer.kind != Slot::Kind::Address) {
        return fail(IrRunError::Kind::Internal, instr,
                    "store to a non-address");
      }
      return write(operand_at(instr, 0).type, value, pointer.address, instr);
    }
    case Opcode::GetElementPtr: return execute_gep(instr, frame);
    case Opcode::IntAdd:
    case Opcode::IntSub:
    case Opcode::IntMul:
    case Opcode::IntAddWrap:
    case Opcode::IntSubWrap:
    case Opcode::IntMulWrap:
    case Opcode::IntDiv:
    case Opcode::UintDiv:
    case Opcode::IntRem:
    case Opcode::UintRem:
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
    case Opcode::ShiftLeft:
    case Opcode::ArithmeticShiftRight:
    case Opcode::LogicalShiftRight: return execute_binary(instr, frame);
    case Opcode::Not: return execute_not(instr, frame);
    case Opcode::BitReverse: return execute_bit_reverse(instr, frame);
    case Opcode::Eq:
    case Opcode::Ne:
    case Opcode::Le:
    case Opcode::Lt:
    case Opcode::Ge:
    case Opcode::Gt: return execute_compare(instr, frame);
    case Opcode::TypeCast: return execute_cast(instr, frame);
    case Opcode::Select: return execute_select(instr, frame);
    case Opcode::Br: {
      const ir::Operand& target_operand = operand_at(instr, 0);
      if (!target_operand.is<ir::BlockIdx>()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "branch without a target");
      }
      const ir::BlockIdx target = target_operand.as_block();
      const ir::Block& target_block = storage_.blocks[target];
      if (target.idx <= block.idx && !spend(instr)) {
        return false;
      }
      if (ins.operands.size() != 1 + target_block.block_params.size()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "branch arity does not match parameters");
      }
      u32 position = 0;
      for (const ir::BlockParamIdx param_id : target_block.block_params) {
        const ir::BlockParam& param = storage_.block_params[param_id];
        Slot value;
        if (!resolve_at(instr, 1 + position, frame, value)) {
          return false;
        }
        Slot bound;
        if (!bind_slot(param.type, value, bound, instr)) {
          return false;
        }
        frame.regs[param.reg.idx] = bound;
        ++position;
      }
      jumped_ = true;
      jump_target_ = target;
      return true;
    }
    case Opcode::CondBr: {
      Slot condition;
      if (!resolve_at(instr, 0, frame, condition)) {
        return false;
      }
      if (condition.kind != Slot::Kind::Scalar) {
        return fail(IrRunError::Kind::Internal, instr,
                    "branch condition is not a scalar");
      }
      const ir::Operand& true_operand = operand_at(instr, 1);
      const ir::Operand& false_operand = operand_at(instr, 2);
      if (!true_operand.is<ir::BlockIdx>() ||
          !false_operand.is<ir::BlockIdx>()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "branch without a target");
      }
      jumped_ = true;
      jump_target_ = condition.bits != 0 ? true_operand.as_block()
                                         : false_operand.as_block();
      return true;
    }
    case Opcode::Call: return execute_call(instr, frame);
    case Opcode::Ret: {
      returned_value_ = Returned{};
      if (!ins.operands.empty()) {
        Slot value;
        if (!resolve_at(instr, 0, frame, value)) {
          return false;
        }
        returned_value_.slot = value;
        returned_value_.type = operand_at(instr, 0).type;
      } else {
        returned_value_.type = ir::primitive_idx(TypeTag::Void);
      }
      returned_ = true;
      return true;
    }
    case Opcode::Unreachable:
      return fail(IrRunError::Kind::Internal, instr,
                  "reached unreachable code");
    case Opcode::ElemOffset:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "element offset in comp evaluation");
    case Opcode::ExtractValue:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "aggregate extraction in comp evaluation");
    case Opcode::InsertValue:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "aggregate insertion in comp evaluation");
    case Opcode::Memcopy:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "memory copy in comp evaluation");
    case Opcode::FAdd:
    case Opcode::FSub:
    case Opcode::FMul:
    case Opcode::FDiv:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "float arithmetic in comp evaluation");
    case Opcode::TypeSizeOf:
    case Opcode::TypeAlignOf:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "type query in comp evaluation");
    case Opcode::Switch:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "switch in comp evaluation");
    case Opcode::AtomicLoad:
    case Opcode::AtomicStore:
    case Opcode::AtomicRmw:
    case Opcode::AtomicCompareExchange:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "atomic operation in comp evaluation");
    case Opcode::Fence:
      return fail(IrRunError::Kind::Unsupported, instr,
                  "fence in comp evaluation");
  }
  return fail(IrRunError::Kind::Internal, instr, "unknown opcode");
}

bool Interpreter::execute_gep(ir::InstructionIdx instr, Frame& frame) {
  const ir::Instruction& ins = storage_.instrs[instr];
  if (ins.operands.size() < 2) {
    return fail(IrRunError::Kind::Internal, instr,
                "getelementptr without an index");
  }
  Slot base;
  if (!resolve_at(instr, 0, frame, base)) {
    return false;
  }
  if (base.kind != Slot::Kind::Address) {
    return fail(IrRunError::Kind::Internal, instr,
                "getelementptr of a non-address");
  }
  ir::TypeIdx current = operand_at(instr, 0).type;
  if (ir::is_pointer_type(tag_of(current))) {
    current = pointee_of(current);
  }
  u64 offset = base.address;
  for (u32 index_at = 1; index_at < ins.operands.size(); ++index_at) {
    u64 index = 0;
    if (!resolve_int_at(instr, index_at, frame, index)) {
      return false;
    }
    if (index_at == 1) {
      offset += index * layout_of(current).size;
      continue;
    }
    const TypeTag tag = tag_of(current);
    if (tag == TypeTag::Struct) {
      const ir::TypeIdxRange fields =
          storage_.struct_types[storage_.types[current].as_struct()].fields;
      if (index >= fields.size()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "getelementptr field out of range");
      }
      offset +=
          ir::field_offset(storage_, fields, static_cast<u32>(index), width_);
      current = fields[static_cast<u32>(index)];
      continue;
    }
    if (tag == TypeTag::Tuple) {
      const ir::TypeIdxRange elements =
          storage_.tuple_types[storage_.types[current].as_tuple()].elements;
      if (index >= elements.size()) {
        return fail(IrRunError::Kind::Internal, instr,
                    "getelementptr element out of range");
      }
      offset +=
          ir::field_offset(storage_, elements, static_cast<u32>(index), width_);
      current = elements[static_cast<u32>(index)];
      continue;
    }
    if (tag == TypeTag::Array) {
      const ir::ArrayType& array =
          storage_.array_types[storage_.types[current].as_array()];
      offset += index * layout_of(array.element).size;
      current = array.element;
      continue;
    }
    return fail(IrRunError::Kind::Unsupported, instr,
                "getelementptr through a non-aggregate in comp evaluation");
  }
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] = Slot{Slot::Kind::Address, 0, offset};
  }
  return true;
}

bool Interpreter::execute_binary(ir::InstructionIdx instr, Frame& frame) {
  const ir::Instruction& ins = storage_.instrs[instr];
  Slot lhs;
  Slot rhs;
  if (!resolve_at(instr, 0, frame, lhs) || !resolve_at(instr, 1, frame, rhs)) {
    return false;
  }
  const TypeTag tag = tag_of(operand_at(instr, 0).type);
  if (!ir::is_integer_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "arithmetic on a non-integer in comp evaluation");
  }
  if (lhs.kind != Slot::Kind::Scalar || rhs.kind != Slot::Kind::Scalar) {
    return fail(IrRunError::Kind::Internal, instr,
                "integer operand is not a scalar");
  }
  const u64 left = lhs.bits & int_mask(tag);
  const u64 right = rhs.bits & int_mask(tag);
  u64 result = 0;
  switch (ins.op) {
    case Opcode::IntAdd:
      if (add_overflows(left, right, tag)) {
        return fail(IrRunError::Kind::Overflow, instr,
                    "integer overflow in comp evaluation");
      }
      result = left + right;
      break;
    case Opcode::IntAddWrap: result = left + right; break;
    case Opcode::IntSub:
      if (sub_overflows(left, right, tag)) {
        return fail(IrRunError::Kind::Overflow, instr,
                    "integer overflow in comp evaluation");
      }
      result = left - right;
      break;
    case Opcode::IntSubWrap: result = left - right; break;
    case Opcode::IntMul:
      if (mul_overflows(left, right, tag)) {
        return fail(IrRunError::Kind::Overflow, instr,
                    "integer overflow in comp evaluation");
      }
      result = left * right;
      break;
    case Opcode::IntMulWrap: result = left * right; break;
    case Opcode::And: result = left & right; break;
    case Opcode::Or: result = left | right; break;
    case Opcode::Xor: result = left ^ right; break;
    case Opcode::ShiftLeft:
      if (right >= 64) {
        return fail(IrRunError::Kind::Unsupported, instr,
                    "shift out of range in comp evaluation");
      }
      result = left << right;
      break;
    case Opcode::LogicalShiftRight:
      if (right >= 64) {
        return fail(IrRunError::Kind::Unsupported, instr,
                    "shift out of range in comp evaluation");
      }
      result = left >> right;
      break;
    case Opcode::ArithmeticShiftRight:
      if (right >= 64) {
        return fail(IrRunError::Kind::Unsupported, instr,
                    "shift out of range in comp evaluation");
      }
      result = is_signed(tag)
                   ? static_cast<u64>(sign_extend(left, tag) >> right)
                   : (left >> right);
      break;
    case Opcode::IntDiv:
    case Opcode::IntRem:
    case Opcode::UintDiv:
    case Opcode::UintRem: {
      if (right == 0) {
        return fail(IrRunError::Kind::Unsupported, instr,
                    "division by zero in comp evaluation");
      }
      if (ins.op == Opcode::UintDiv || ins.op == Opcode::UintRem) {
        result = ins.op == Opcode::UintDiv ? left / right : left % right;
        break;
      }
      const i64 signed_left = sign_extend(left, tag);
      const i64 signed_right = sign_extend(right, tag);
      constexpr i64 MIN = static_cast<i64>(static_cast<u64>(1) << 63);
      if (signed_left == MIN && signed_right == -1 && int_bytes(tag) == 8) {
        result = static_cast<u64>(MIN);
        break;
      }
      result = ins.op == Opcode::IntDiv
                   ? static_cast<u64>(signed_left / signed_right)
                   : static_cast<u64>(signed_left % signed_right);
      break;
    }
    default:
      return fail(IrRunError::Kind::Internal, instr,
                  "opcode is not a binary operation");
  }
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] =
        Slot{Slot::Kind::Scalar, result & int_mask(tag), 0};
  }
  return true;
}

bool Interpreter::execute_not(ir::InstructionIdx instr, Frame& frame) {
  Slot value;
  if (!resolve_at(instr, 0, frame, value)) {
    return false;
  }
  const TypeTag tag = tag_of(operand_at(instr, 0).type);
  if (!ir::is_integer_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "bitwise not on a non-integer in comp evaluation");
  }
  if (value.kind != Slot::Kind::Scalar) {
    return fail(IrRunError::Kind::Internal, instr,
                "integer operand is not a scalar");
  }
  const ir::Instruction& ins = storage_.instrs[instr];
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] =
        Slot{Slot::Kind::Scalar, ~value.bits & int_mask(tag), 0};
  }
  return true;
}

bool Interpreter::execute_bit_reverse(ir::InstructionIdx instr, Frame& frame) {
  Slot value;
  if (!resolve_at(instr, 0, frame, value)) {
    return false;
  }
  const TypeTag tag = tag_of(operand_at(instr, 0).type);
  if (!ir::is_integer_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "bit reverse on a non-integer in comp evaluation");
  }
  if (value.kind != Slot::Kind::Scalar) {
    return fail(IrRunError::Kind::Internal, instr,
                "integer operand is not a scalar");
  }
  const u32 width = int_width_bits(tag);
  const u64 bits = value.bits & int_mask(tag);
  u64 reversed = 0;
  for (u32 i = 0; i < width; ++i) {
    if (((bits >> i) & 1) != 0) {
      reversed |= static_cast<u64>(1) << (width - 1 - i);
    }
  }
  const ir::Instruction& ins = storage_.instrs[instr];
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] = Slot{Slot::Kind::Scalar, reversed, 0};
  }
  return true;
}

bool Interpreter::execute_compare(ir::InstructionIdx instr, Frame& frame) {
  Slot lhs;
  Slot rhs;
  if (!resolve_at(instr, 0, frame, lhs) || !resolve_at(instr, 1, frame, rhs)) {
    return false;
  }
  const ir::Instruction& ins = storage_.instrs[instr];
  const TypeTag tag = tag_of(operand_at(instr, 0).type);
  bool outcome = false;
  if (ir::is_integer_type(tag)) {
    if (lhs.kind != Slot::Kind::Scalar || rhs.kind != Slot::Kind::Scalar) {
      return fail(IrRunError::Kind::Internal, instr,
                  "integer operand is not a scalar");
    }
    const u64 left = lhs.bits & int_mask(tag);
    const u64 right = rhs.bits & int_mask(tag);
    switch (ins.op) {
      case Opcode::Eq: outcome = left == right; break;
      case Opcode::Ne: outcome = left != right; break;
      case Opcode::Le:
      case Opcode::Lt:
      case Opcode::Ge:
      case Opcode::Gt:
        if (is_signed(tag)) {
          const i64 signed_left = sign_extend(left, tag);
          const i64 signed_right = sign_extend(right, tag);
          outcome = ins.op == Opcode::Le   ? signed_left <= signed_right
                    : ins.op == Opcode::Lt ? signed_left < signed_right
                    : ins.op == Opcode::Ge ? signed_left >= signed_right
                                           : signed_left > signed_right;
        } else {
          outcome = ins.op == Opcode::Le   ? left <= right
                    : ins.op == Opcode::Lt ? left < right
                    : ins.op == Opcode::Ge ? left >= right
                                           : left > right;
        }
        break;
      default:
        return fail(IrRunError::Kind::Internal, instr,
                    "opcode is not a comparison");
    }
  } else if (ir::is_pointer_type(tag)) {
    if (ins.op != Opcode::Eq && ins.op != Opcode::Ne) {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "ordering of references in comp evaluation");
    }
    if (lhs.kind != Slot::Kind::Address || rhs.kind != Slot::Kind::Address) {
      return fail(IrRunError::Kind::Internal, instr,
                  "reference operand is not an address");
    }
    const bool equal = lhs.address == rhs.address;
    outcome = ins.op == Opcode::Eq ? equal : !equal;
  } else if (ir::is_float_type(tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "float comparison in comp evaluation");
  } else {
    return fail(IrRunError::Kind::Unsupported, instr,
                "comparison of a non-integer in comp evaluation");
  }
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] =
        Slot{Slot::Kind::Scalar, static_cast<u64>(outcome ? 1 : 0), 0};
  }
  return true;
}

bool Interpreter::execute_cast(ir::InstructionIdx instr, Frame& frame) {
  const ir::Instruction& ins = storage_.instrs[instr];
  if (!ins.dst.is_valid()) {
    return fail(IrRunError::Kind::Internal, instr,
                "cast without a destination");
  }
  Slot source;
  if (!resolve_at(instr, 0, frame, source)) {
    return false;
  }
  const ir::TypeIdx source_type = operand_at(instr, 0).type;
  const ir::TypeIdx target_type = reg_type(ins.dst);
  const TypeTag source_tag = tag_of(source_type);
  const TypeTag target_tag = tag_of(target_type);
  if (source_tag == target_tag) {
    frame.regs[ins.dst.idx] = source;
    return true;
  }
  if (ir::is_integer_type(source_tag) && ir::is_integer_type(target_tag)) {
    if (source.kind != Slot::Kind::Scalar) {
      return fail(IrRunError::Kind::Internal, instr,
                  "integer operand is not a scalar");
    }
    u64 bits = source.bits & int_mask(source_tag);
    if (target_tag == TypeTag::I1) {
      bits = bits != 0 ? 1 : 0;
    } else {
      bits &= int_mask(target_tag);
      if (is_signed(target_tag) && is_signed(source_tag)) {
        const u32 bytes = int_bytes(target_tag);
        const u64 sign = bytes >= 8 ? static_cast<u64>(1) << 63
                                    : static_cast<u64>(1) << (bytes * 8 - 1);
        if ((bits & sign) != 0) {
          bits |= ~int_mask(target_tag);
        }
      }
    }
    frame.regs[ins.dst.idx] = Slot{Slot::Kind::Scalar, bits, 0};
    return true;
  }
  if (ir::is_pointer_type(source_tag) && ir::is_pointer_type(target_tag)) {
    if (source.kind != Slot::Kind::Address) {
      return fail(IrRunError::Kind::Internal, instr,
                  "reference operand is not an address");
    }
    if (layout_of(source_type).size != word_bytes() ||
        layout_of(target_type).size != word_bytes()) {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "cast of a fat reference in comp evaluation");
    }
    frame.regs[ins.dst.idx] = source;
    return true;
  }
  if (ir::is_float_type(source_tag) || ir::is_float_type(target_tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "float cast in comp evaluation");
  }
  if (ir::is_pointer_type(source_tag) || ir::is_pointer_type(target_tag)) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "cast between a reference and an integer in comp evaluation");
  }
  return fail(IrRunError::Kind::Unsupported, instr,
              "cast between these types in comp evaluation");
}

bool Interpreter::execute_select(ir::InstructionIdx instr, Frame& frame) {
  Slot condition;
  Slot on_true;
  Slot on_false;
  if (!resolve_at(instr, 0, frame, condition) ||
      !resolve_at(instr, 1, frame, on_true) ||
      !resolve_at(instr, 2, frame, on_false)) {
    return false;
  }
  if (condition.kind != Slot::Kind::Scalar) {
    return fail(IrRunError::Kind::Internal, instr,
                "select condition is not a scalar");
  }
  if (on_true.kind != on_false.kind) {
    return fail(IrRunError::Kind::Internal, instr,
                "select of different value kinds");
  }
  const ir::Instruction& ins = storage_.instrs[instr];
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] = condition.bits != 0 ? on_true : on_false;
  }
  return true;
}

bool Interpreter::execute_call(ir::InstructionIdx instr, Frame& frame) {
  const ir::Instruction& ins = storage_.instrs[instr];
  const ir::Operand& callee = operand_at(instr, 0);
  if (callee.is<ir::ExternalFunctionIdx>()) {
    const ir::ExternalFunction& external =
        storage_.external_functions[callee.as_external_function()];
    const std::string_view name = strings_.get(external.meta.name);
    if (name == "alcy_panic") {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "call to panic in comp evaluation");
    }
    if (name == "alcy_print" || name == "alcy_println") {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "call to print in comp evaluation");
    }
    if (name == "alcy_alloc") {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "allocation in comp evaluation");
    }
    if (name == "alcy_dealloc") {
      return fail(IrRunError::Kind::Unsupported, instr,
                  "deallocation in comp evaluation");
    }
    return fail(IrRunError::Kind::Unsupported, instr,
                "call to a runtime function in comp evaluation");
  }
  if (!callee.is<ir::FunctionIdx>()) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "indirect call in comp evaluation");
  }
  const ir::FunctionIdx target = callee.as_function();
  // A caller may hold storage that is still being built (the lowering's
  // seam asks mid-pass), so a target without a function record yet is
  // refused rather than read. A finished, verified storage can never
  // take this path.
  if (target.idx >= storage_.functions.size()) {
    return fail(IrRunError::Kind::Unsupported, instr,
                "call to a function that is not lowered yet");
  }
  const ir::Function& fn = storage_.functions[target];
  const ir::Block& entry = storage_.blocks[fn.blocks.head()];
  if (ins.operands.size() - 1 != entry.block_params.size()) {
    return fail(IrRunError::Kind::Internal, instr,
                "call arity does not match parameters");
  }
  std::vector<Slot> args;
  args.reserve(entry.block_params.size());
  for (u32 offset = 1; offset < ins.operands.size(); ++offset) {
    Slot value;
    if (!resolve_at(instr, offset, frame, value)) {
      return false;
    }
    args.push_back(value);
  }
  Returned returned;
  if (!execute_function(target, args, instr, returned)) {
    return false;
  }
  // The callee's terminator left its flow in the shared slots; this call
  // is not itself a return or a branch of the caller.
  jumped_ = false;
  returned_ = false;
  if (ins.dst.is_valid()) {
    frame.regs[ins.dst.idx] = returned.slot;
  }
  return true;
}

base::Result<CompVal, IrRunError> Interpreter::to_result(
    const Returned& returned) {
  const TypeTag tag = tag_of(returned.type);
  CompValue value;
  if (tag == TypeTag::Void) {
    return base::make_ok(CompVal{std::move(value), returned.type});
  }
  if (tag == TypeTag::I1) {
    if (returned.slot.kind != Slot::Kind::Scalar) {
      fail(IrRunError::Kind::Internal, ir::InstructionIdx(base::INVALID_IDX),
           "boolean result is not a scalar");
      return error_result();
    }
    value.tag = CompValue::Tag::Bool;
    value.bool_value = returned.slot.bits != 0;
    return base::make_ok(CompVal{std::move(value), returned.type});
  }
  if (ir::is_integer_type(tag)) {
    if (returned.slot.kind != Slot::Kind::Scalar) {
      fail(IrRunError::Kind::Internal, ir::InstructionIdx(base::INVALID_IDX),
           "integer result is not a scalar");
      return error_result();
    }
    value.tag = CompValue::Tag::Int;
    value.int_value = returned.slot.bits & int_mask(tag);
    return base::make_ok(CompVal{std::move(value), returned.type});
  }
  if (ir::is_pointer_type(tag)) {
    fail(IrRunError::Kind::Unsupported, ir::InstructionIdx(base::INVALID_IDX),
         "reference result in comp evaluation");
    return error_result();
  }
  if (ir::is_float_type(tag)) {
    fail(IrRunError::Kind::Unsupported, ir::InstructionIdx(base::INVALID_IDX),
         "float result in comp evaluation");
    return error_result();
  }
  fail(IrRunError::Kind::Unsupported, ir::InstructionIdx(base::INVALID_IDX),
       "aggregate result in comp evaluation");
  return error_result();
}

}  // namespace

base::Result<CompVal, IrRunError> interpret_function(
    const ir::StorageState& storage,
    const ir::SymbolTable& strings,
    ir::PointerWidth width,
    ir::FunctionIdx function,
    std::span<const CompVal> args,
    u64& quota,
    u32& depth) {
  Interpreter interpreter(storage, strings, width, quota, depth);
  return interpreter.run(function, args);
}

}  // namespace comp
