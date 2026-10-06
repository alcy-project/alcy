// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>

#include "codegen/backend.h"
#include "codegen/wasm/emit.h"
#include "codegen/wasm/opcodes.h"
#include "codegen/wasm/writer.h"
#include "debug/dcheck.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/common.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace codegen::wasm {
namespace {

using op::OP_DROP;
using op::OP_I32_ADD;
using op::OP_I32_AND;
using op::OP_I32_MUL;

// The load and store opcode, with the alignment hint, for a scalar. The
// narrow forms read and write exactly the bytes the IR type's layout
// promised, so a `u8` field never disturbs its neighbours.
struct Access {
  u8 load = 0;
  u32 load_align = 0;
  u8 store = 0;
  u32 store_align = 0;
};

std::optional<Access> access_for(ir::TypeTag tag) {
  using T = ir::TypeTag;
  switch (tag) {
    case T::I1: return Access{0x2D, 0, 0x3A, 0};  // load8_u / store8
    case T::I8: return Access{0x2C, 0, 0x3A, 0};
    case T::U8: return Access{0x2D, 0, 0x3A, 0};
    case T::I16: return Access{0x2E, 1, 0x3B, 1};
    case T::U16: return Access{0x2F, 1, 0x3B, 1};
    case T::I32:
    case T::U32: return Access{0x28, 2, 0x36, 2};
    case T::I64:
    case T::U64: return Access{0x29, 3, 0x37, 3};
    case T::F32: return Access{0x2A, 2, 0x38, 2};
    case T::F64: return Access{0x2B, 3, 0x39, 3};
    case T::Ptr:
    case T::Function:
    case T::Ref:
    case T::MutRef: return Access{0x28, 2, 0x36, 2};
    default: return std::nullopt;
  }
}

bool is_aggregate_tag(ir::TypeTag tag) {
  using T = ir::TypeTag;
  return tag == T::Struct || tag == T::Tuple || tag == T::Enum ||
         tag == T::Array;
}

}  // namespace

Emitter::EmitResult Emitter::emit_memory(const ir::Instruction& instr) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;

  switch (instr.op) {
    case O::Alloca: {
      DCHECK_EQ(ops.size(), 1u);
      if (!instr.dst.is_valid()) {
        return base::make_ok();
      }
      const ir::TypeIdx elem = storage_.registers()[instr.dst].type;
      const ir::TypeLayout layout = storage_.layout_of(elem, target_.width);
      const u32 size = layout.size == 0 ? 1 : static_cast<u32>(layout.size);
      const u32 align = layout.align == 0 ? 1 : static_cast<u32>(layout.align);
      alloca_elem_[instr.dst.idx] = elem;

      const ir::Operand& count = storage_.operands()[ops.head()];
      const std::optional<ValueShape> count_shape = shape_of(count.type);
      if (!count_shape.has_value() || count_shape->words != 1 ||
          count_shape->type != ValType::I32) {
        return unsupported(current_span_, "a variable-length allocation");
      }

      // scratch0 = align_up(sp, align)
      global_get(GLOBAL_SP);
      i32_const(static_cast<i32>(align - 1));
      op(OP_I32_ADD);
      i32_const(static_cast<i32>(0u - align));
      op(OP_I32_AND);
      local_set(scratch0_);
      // sp = scratch0 + size * count
      local_get(scratch0_);
      if (push_word(count, 0, ValType::I32).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      i32_const(static_cast<i32>(size));
      op(OP_I32_MUL);
      op(OP_I32_ADD);
      global_set(GLOBAL_SP);
      local_get(scratch0_);
      local_set(reg_base_[instr.dst.idx]);
      return base::make_ok();
    }
    case O::Load: {
      DCHECK_EQ(ops.size(), 1u);
      if (!instr.dst.is_valid()) {
        return base::make_ok();
      }
      const ir::Operand& ptr = storage_.operands()[ops.head()];
      const ir::TypeIdx type = storage_.registers()[instr.dst].type;
      const ir::TypeTag tag = storage_.types()[type].tag;
      const u32 base = reg_base_[instr.dst.idx];

      if (is_aggregate_tag(tag)) {
        const u32 slot = reg_slot_[instr.dst.idx];
        DCHECK(slot != NO_LOCAL);
        const u32 size =
            static_cast<u32>(storage_.layout_of(type, target_.width).size);
        local_get(frame_local_);
        i32_const(static_cast<i32>(slot));
        op(OP_I32_ADD);
        if (push_word(ptr, 0, ValType::I32).is_err()) {
          return base::make_err(codegen::EmitError::Unsupported);
        }
        i32_const(static_cast<i32>(size));
        body_.bytes(codegen::wasm::MEMORY_COPY);
        local_get(frame_local_);
        i32_const(static_cast<i32>(slot));
        op(OP_I32_ADD);
        local_set(base);
        return base::make_ok();
      }

      if (tag == ir::TypeTag::Str || tag == ir::TypeTag::Slice ||
          (tag == ir::TypeTag::Ref &&
           storage_.types()[storage_
                                .ref_types()[storage_.types()[type].as_ref()]
                                .pointee]
                   .tag == ir::TypeTag::Slice) ||
          (tag == ir::TypeTag::MutRef &&
           storage_.types()[storage_
                                .ref_types()[storage_.types()[type].as_ref()]
                                .pointee]
                   .tag == ir::TypeTag::Slice)) {
        // A fat pointer: two words, both loaded from the pair.
        for (u32 word = 0; word < 2; ++word) {
          if (push_word(ptr, 0, ValType::I32).is_err()) {
            return base::make_err(codegen::EmitError::Unsupported);
          }
          i32_load(body_, 2, word * 4);
          local_set(base + word);
        }
        return base::make_ok();
      }

      const std::optional<Access> access = access_for(tag);
      if (!access.has_value()) {
        return unsupported(current_span_, ir::type_to_str(tag));
      }
      if (push_word(ptr, 0, ValType::I32).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      body_.byte(access->load);
      body_.u32_leb(access->load_align);
      body_.u32_leb(0);
      if (tag == ir::TypeTag::I1) {
        i32_const(1);
        op(OP_I32_AND);
      }
      local_set(base);
      return base::make_ok();
    }
    case O::Store: {
      DCHECK_EQ(ops.size(), 2u);
      const ir::Operand& value = storage_.operands()[ops.head()];
      const ir::Operand& ptr = storage_.operands()[ops.head() + 1];
      const ir::TypeTag tag = storage_.types()[value.type].tag;

      if (is_aggregate_tag(tag)) {
        const u32 size = static_cast<u32>(
            storage_.layout_of(value.type, target_.width).size);
        if (push_word(ptr, 0, ValType::I32).is_err() ||
            push_word(value, 0, ValType::I32).is_err()) {
          return base::make_err(codegen::EmitError::Unsupported);
        }
        i32_const(static_cast<i32>(size));
        body_.bytes(codegen::wasm::MEMORY_COPY);
        return base::make_ok();
      }

      if (tag == ir::TypeTag::Str || tag == ir::TypeTag::Slice ||
          (tag == ir::TypeTag::Ref &&
           storage_.types()
                   [storage_.ref_types()[storage_.types()[value.type].as_ref()]
                        .pointee]
                       .tag == ir::TypeTag::Slice) ||
          (tag == ir::TypeTag::MutRef &&
           storage_.types()
                   [storage_.ref_types()[storage_.types()[value.type].as_ref()]
                        .pointee]
                       .tag == ir::TypeTag::Slice)) {
        for (u32 word = 0; word < 2; ++word) {
          if (push_word(ptr, 0, ValType::I32).is_err() ||
              push_word(value, word, ValType::I32).is_err()) {
            return base::make_err(codegen::EmitError::Unsupported);
          }
          i32_store(body_, 2, word * 4);
        }
        return base::make_ok();
      }

      const std::optional<Access> access = access_for(tag);
      if (!access.has_value()) {
        return unsupported(current_span_, ir::type_to_str(tag));
      }
      if (push_word(ptr, 0, ValType::I32).is_err() ||
          push_operand(value).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      body_.byte(access->store);
      body_.u32_leb(access->store_align);
      body_.u32_leb(0);
      return base::make_ok();
    }
    case O::Memcopy: {
      DCHECK_EQ(ops.size(), 3u);
      for (u32 index = 0; index < 3; ++index) {
        const ir::Operand& operand = storage_.operands()[ops.head() + index];
        if (push_word(operand, 0, ValType::I32).is_err()) {
          return base::make_err(codegen::EmitError::Unsupported);
        }
      }
      body_.bytes(codegen::wasm::MEMORY_COPY);
      return base::make_ok();
    }
    case O::GetElementPtr: {
      const ir::Operand& base = storage_.operands()[ops.head()];
      DCHECK(base.is<ir::RegisterIdx>());
      const ir::RegisterIdx base_reg = base.as_register();
      ir::TypeIdx elem = alloca_elem_[base_reg.idx];
      if (!elem.is_valid()) {
        const ir::TypeIdx base_type = storage_.registers()[base_reg].type;
        const ir::TypeTag tag = storage_.types()[base_type].tag;
        if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
          return unsupported(current_span_, "a projection of this pointer");
        }
        elem =
            storage_.ref_types()[storage_.types()[base_type].as_ref()].pointee;
      }
      if (ops.size() == 2) {
        return emit_elem_offset(base, storage_.operands()[ops.head() + 1], elem,
                                instr);
      }
      return emit_field_projection(base, elem, ops, instr);
    }
    case O::ElemOffset: {
      DCHECK_EQ(ops.size(), 2u);
      DCHECK(instr.dst.is_valid());
      const ir::TypeIdx dst_type = storage_.registers()[instr.dst].type;
      const ir::TypeTag tag = storage_.types()[dst_type].tag;
      DCHECK(tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef);
      const ir::TypeIdx elem =
          storage_.ref_types()[storage_.types()[dst_type].as_ref()].pointee;
      return emit_elem_offset(storage_.operands()[ops.head()],
                              storage_.operands()[ops.head() + 1], elem, instr);
    }
    case O::ExtractValue: {
      if (ops.size() < 2) {
        return unsupported(current_span_, "this extraction");
      }
      const ir::Operand& aggregate = storage_.operands()[ops.head()];
      if (is_aggregate_tag(storage_.types()[aggregate.type].tag)) {
        return emit_field_load(aggregate, ops, instr);
      }
      if (ops.size() != 2) {
        return unsupported(current_span_, "this extraction");
      }
      const ir::Operand& index_op = storage_.operands()[ops.head() + 1];
      if (!index_op.is<ir::ImmutableIdx>()) {
        return unsupported(current_span_, "this extraction");
      }
      const ir::Immutable& index =
          storage_.immutables()[index_op.as_immutable()];
      const ir::TypeTag index_tag = storage_.types()[index.type].tag;
      const u64 field = index.as_u64_integer(index_tag);
      const std::optional<ValueShape> shape = shape_of(aggregate.type);
      if (!shape.has_value() || shape->words != 2 || field > 1) {
        return unsupported(current_span_, "this extraction");
      }
      if (!instr.dst.is_valid()) {
        return base::make_ok();
      }
      if (push_word(aggregate, static_cast<u32>(field), ValType::I32)
              .is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      local_set(reg_base_[instr.dst.idx]);
      return base::make_ok();
    }
    case O::InsertValue:
      return unsupported(current_span_, "an aggregate construction");
    default: return unsupported(current_span_, ir::opcode_to_str(instr.op));
  }
}

// base + index * sizeof(elem), the one projection the MVP encodes.
Emitter::EmitResult Emitter::emit_elem_offset(const ir::Operand& base,
                                              const ir::Operand& index,
                                              ir::TypeIdx elem,
                                              const ir::Instruction& instr) {
  const ir::TypeLayout layout = storage_.layout_of(elem, target_.width);
  const u32 stride = layout.size == 0 ? 1 : static_cast<u32>(layout.size);
  if (push_word(base, 0, ValType::I32).is_err() ||
      push_word(index, 0, ValType::I32).is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  i32_const(static_cast<i32>(stride));
  op(OP_I32_MUL);
  op(OP_I32_ADD);
  if (instr.dst.is_valid() && reg_shape_[instr.dst.idx].words > 0) {
    local_set(reg_base_[instr.dst.idx]);
  } else {
    op(OP_DROP);
  }
  return base::make_ok();
}

// base + the folded field offset, the projection the lowering writes for
// `t.0` and its siblings.
Emitter::EmitResult Emitter::emit_field_projection(
    const ir::Operand& base,
    ir::TypeIdx elem,
    ir::OperandIdxRange ops,
    const ir::Instruction& instr) {
  ir::TypeIdx cur = elem;
  u64 constant = 0;
  const ir::Operand* dynamic = nullptr;
  u32 stride = 0;
  for (u32 i = 1; i < ops.size(); ++i) {
    const ir::Operand& index_op = storage_.operands()[ops.head() + i];
    const ir::TypeTag tag = storage_.types()[cur].tag;
    u64 value = 0;
    bool is_const = false;
    if (index_op.is<ir::ImmutableIdx>()) {
      const ir::Immutable& imm = storage_.immutables()[index_op.as_immutable()];
      value = imm.as_u64_integer(storage_.types()[imm.type].tag);
      is_const = true;
    }
    if (i == 1) {
      const u64 size = storage_.layout_of(cur, target_.width).size;
      if (is_const) {
        constant += value * size;
      } else if (dynamic == nullptr) {
        dynamic = &index_op;
        stride = size == 0 ? 1 : static_cast<u32>(size);
      } else {
        return unsupported(current_span_, "this projection");
      }
      continue;
    }
    if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Tuple) {
      if (!is_const) {
        return unsupported(current_span_, "a dynamic field index");
      }
      const ir::TypeNode& node = storage_.types()[cur];
      const ir::TypeIdxRange fields =
          tag == ir::TypeTag::Struct
              ? storage_.struct_types()[node.as_struct()].fields
              : storage_.tuple_types()[node.as_tuple()].elements;
      constant += ir::field_offset(storage_.state(), fields,
                                   static_cast<u32>(value), target_.width);
      cur = fields[static_cast<u32>(value)];
      continue;
    }
    if (tag == ir::TypeTag::Array) {
      const ir::ArrayType& array =
          storage_.array_types()[storage_.types()[cur].as_array()];
      const u64 size = storage_.layout_of(array.element, target_.width).size;
      if (is_const) {
        constant += value * size;
      } else if (dynamic == nullptr) {
        dynamic = &index_op;
        stride = size == 0 ? 1 : static_cast<u32>(size);
      } else {
        return unsupported(current_span_, "this projection");
      }
      cur = array.element;
      continue;
    }
    return unsupported(current_span_, "this projection");
  }

  if (push_word(base, 0, ValType::I32).is_err()) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  if (constant != 0) {
    i32_const(static_cast<i32>(static_cast<u32>(constant)));
    op(OP_I32_ADD);
  }
  if (dynamic != nullptr) {
    if (push_word(*dynamic, 0, ValType::I32).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    i32_const(static_cast<i32>(stride));
    op(OP_I32_MUL);
    op(OP_I32_ADD);
  }
  if (instr.dst.is_valid() && reg_shape_[instr.dst.idx].words > 0) {
    local_set(reg_base_[instr.dst.idx]);
  } else {
    op(OP_DROP);
  }
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_field_load(const ir::Operand& aggregate,
                                             ir::OperandIdxRange ops,
                                             const ir::Instruction& instr) {
  u32 offset = 0;
  ir::TypeIdx cur = aggregate.type;
  for (u32 i = 1; i < ops.size(); ++i) {
    const ir::Operand& index_op = storage_.operands()[ops.head() + i];
    if (!index_op.is<ir::ImmutableIdx>()) {
      return unsupported(current_span_, "a dynamic field index");
    }
    const ir::Immutable& imm = storage_.immutables()[index_op.as_immutable()];
    const u64 value = imm.as_u64_integer(storage_.types()[imm.type].tag);
    const ir::TypeTag tag = storage_.types()[cur].tag;
    if (tag != ir::TypeTag::Struct && tag != ir::TypeTag::Tuple) {
      return unsupported(current_span_, "this extraction");
    }
    const ir::TypeNode& node = storage_.types()[cur];
    const ir::TypeIdxRange fields =
        tag == ir::TypeTag::Struct
            ? storage_.struct_types()[node.as_struct()].fields
            : storage_.tuple_types()[node.as_tuple()].elements;
    offset += static_cast<u32>(ir::field_offset(
        storage_.state(), fields, static_cast<u32>(value), target_.width));
    cur = fields[static_cast<u32>(value)];
  }
  if (!instr.dst.is_valid()) {
    return base::make_ok();
  }
  const std::optional<ValueShape> shape = shape_of(cur);
  const u32 base = reg_base_[instr.dst.idx];
  if (shape.has_value() && shape->words == 1) {
    const std::optional<Access> access = access_for(storage_.types()[cur].tag);
    if (!access.has_value()) {
      return unsupported(current_span_, "this extraction");
    }
    if (push_word(aggregate, 0, ValType::I32).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    body_.byte(access->load);
    body_.u32_leb(access->load_align);
    body_.u32_leb(offset);
    if (storage_.types()[cur].tag == ir::TypeTag::I1) {
      i32_const(1);
      op(OP_I32_AND);
    }
    local_set(base);
    return base::make_ok();
  }
  if (shape.has_value() && shape->words == 2) {
    for (u32 word = 0; word < 2; ++word) {
      if (push_word(aggregate, 0, ValType::I32).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      codegen::wasm::i32_load(body_, 2, offset + word * 4);
      local_set(base + word);
    }
    return base::make_ok();
  }
  return unsupported(current_span_, "this extraction");
}

}  // namespace codegen::wasm
