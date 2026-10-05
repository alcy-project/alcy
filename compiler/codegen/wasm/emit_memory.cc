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

constexpr u8 MEMORY_COPY[4] = {0xFC, 0x0A, 0x00, 0x00};

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
      body_.bytes(MEMORY_COPY);
      return base::make_ok();
    }
    case O::GetElementPtr: {
      if (ops.size() != 2) {
        return unsupported(current_span_, "a multi-index projection");
      }
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
      return emit_elem_offset(base, storage_.operands()[ops.head() + 1], elem,
                              instr);
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
      if (ops.size() != 2) {
        return unsupported(current_span_, "this extraction");
      }
      const ir::Operand& aggregate = storage_.operands()[ops.head()];
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

}  // namespace codegen::wasm
