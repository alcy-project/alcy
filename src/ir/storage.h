// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <utility>

#include "debug/fatal.h"
#include "fpag/base/vec.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/type.h"

namespace ir {

struct StorageState {
  // Note: base::Vec maintained (not arena) because idx stability,
  // random access, and DOD iteration favor dense table; arena conversion
  // offers no benefit for IR storage.

  template <typename T>
  using Alloc = std::allocator<T>;

  using Functions = base::Vec<Function, FunctionIdx, Alloc<Function>>;
  using Blocks = base::Vec<Block, BlockIdx, Alloc<Block>>;
  using BlockParams = base::Vec<BlockParam, BlockParamIdx, Alloc<BlockParam>>;
  using Instructions =
      base::Vec<Instruction, InstructionIdx, Alloc<Instruction>>;
  using Immutables = base::Vec<Immutable, ImmutableIdx, Alloc<Immutable>>;
  using Registers = base::Vec<Register, RegisterIdx, Alloc<Register>>;
  using ExternalFunctions =
      base::Vec<ExternalFunction, ExternalFunctionIdx, Alloc<ExternalFunction>>;
  using Operands = base::Vec<Operand, OperandIdx, Alloc<Operand>>;
  using Types = base::Vec<TypeNode, TypeIdx, Alloc<TypeNode>>;
  using StructTypes = base::Vec<StructType, StructTypeIdx, Alloc<StructType>>;
  using ArrayTypes = base::Vec<ArrayType, ArrayTypeIdx, Alloc<ArrayType>>;
  using EnumTypes = base::Vec<EnumType, EnumTypeIdx, Alloc<EnumType>>;
  using EnumVariantTypes =
      base::Vec<EnumVariantType, EnumVariantTypeIdx, Alloc<EnumVariantType>>;
  using RefTypes = base::Vec<RefType, RefTypeIdx, Alloc<RefType>>;
  using TupleTypes = base::Vec<TupleType, TupleTypeIdx, Alloc<TupleType>>;

  Functions functions;
  Blocks blocks;
  BlockParams block_params;
  Instructions instrs;
  Immutables immutables;
  Registers registers;
  ExternalFunctions external_functions;
  Operands operands;
  Types types;
  StructTypes struct_types;
  ArrayTypes array_types;
  EnumTypes enum_types;
  EnumVariantTypes enum_variant_types;
  RefTypes ref_types;
  TupleTypes tuple_types;
};

// Size and alignment of a type on a target, in bytes.
//
// One source of layout knowledge for the whole compiler: the emitter
// asserts against it when it builds a type, and a region solver needs
// the same numbers. The rules are LLVM's default data layout, so a
// field list lays its fields out in order, each at the next offset its
// own alignment allows.
struct TypeLayout {
  u64 size = 0;
  u64 align = 1;
};

constexpr u64 align_up(u64 value, u64 align) {
  return align == 0 ? value : ((value + align - 1) / align) * align;
}

inline TypeLayout type_layout(const StorageState& state,
                              TypeIdx idx,
                              PointerWidth width);
inline TypeLayout fields_layout(const StorageState& state,
                                TypeIdxRange fields,
                                PointerWidth width);
inline TypeLayout enum_payload_area(const StorageState& state,
                                    TypeIdx idx,
                                    PointerWidth width);

// Layout of a field list: each field at the next offset its alignment
// allows, the whole rounded up to the strictest field. An empty list has
// no size, which is what an uninhabited payload needs.
inline TypeLayout fields_layout(const StorageState& state,
                                TypeIdxRange fields,
                                PointerWidth width) {
  TypeLayout out{};
  if (fields.empty()) {
    return out;
  }
  for (TypeIdx field : fields) {
    const TypeLayout field_layout = type_layout(state, field, width);
    out.size = align_up(out.size, field_layout.align) + field_layout.size;
    out.align = field_layout.align > out.align ? field_layout.align : out.align;
  }
  out.size = align_up(out.size, out.align);
  return out;
}

// Byte offset of field `index` within a field list laid out by
// fields_layout. The same walk, so an offset and the size that reserved
// it cannot disagree.
inline u64 field_offset(const StorageState& state,
                        TypeIdxRange fields,
                        u32 index,
                        PointerWidth width) {
  u64 cursor = 0;
  for (u32 i = 0; i < index; ++i) {
    const TypeLayout layout = type_layout(state, fields[i], width);
    cursor = align_up(cursor, layout.align) + layout.size;
  }
  return align_up(cursor, type_layout(state, fields[index], width).align);
}

// The payload area of an enum's slot: a discriminant, then room for the
// widest variant payload, aligned for the strictest payload field. The
// size is a whole number of carriers so the emitted array has exactly
// this layout and a field's offset within the area is the same number
// the lowerer and the emitter both read.
inline TypeLayout enum_payload_area(const StorageState& state,
                                    TypeIdx idx,
                                    PointerWidth width) {
  u64 align = 1;
  u64 size = 0;
  const EnumType& enum_type = state.enum_types[state.types[idx].as_enum()];
  for (EnumVariantTypeIdx variant : enum_type.variants) {
    const TypeIdxRange fields = state.enum_variant_types[variant].fields;
    if (fields.empty()) {
      continue;
    }
    for (TypeIdx field : fields) {
      const u64 field_align = type_layout(state, field, width).align;
      if (field_align > align) {
        align = field_align;
      }
    }
    const u64 variant_size = fields_layout(state, fields, width).size;
    if (variant_size > size) {
      size = variant_size;
    }
  }
  return {align_up(size, align), align};
}

inline TypeLayout type_layout(const StorageState& state,
                              TypeIdx idx,
                              PointerWidth width) {
  const u64 word = width == PointerWidth::W64 ? 8 : 4;
  const TypeNode& node = state.types[idx];
  switch (node.tag) {
    // Uninhabited: a `()` field stores nothing, so it takes no room.
    case TypeTag::Void:
    case TypeTag::Never:
    case TypeTag::Error: return {};
    case TypeTag::I1: return {1, 1};
    case TypeTag::I8:
    case TypeTag::U8: return {1, 1};
    case TypeTag::I16:
    case TypeTag::U16: return {2, 2};
    case TypeTag::I32:
    case TypeTag::U32:
    case TypeTag::F32: return {4, 4};
    case TypeTag::I64:
    case TypeTag::U64:
    case TypeTag::F64: return {8, 8};
    // Fat pointer: {bytes, len}, with the length riding the target.
    case TypeTag::Str: return {2 * word, word};
    case TypeTag::Ptr:
    case TypeTag::Ref:
    case TypeTag::MutRef:
    case TypeTag::Function: return {word, word};
    case TypeTag::Struct:
      return fields_layout(state, state.struct_types[node.as_struct()].fields,
                           width);
    case TypeTag::Tuple:
      return fields_layout(state, state.tuple_types[node.as_tuple()].elements,
                           width);
    case TypeTag::Array: {
      const ArrayType& array = state.array_types[node.as_array()];
      if (array.count == 0) {
        return {};
      }
      const TypeLayout element = type_layout(state, array.element, width);
      return {element.size * array.count, element.align};
    }
    case TypeTag::Enum: {
      // A discriminant, then the payload area. The area is a whole
      // number of carriers, so the emitted slot has exactly this layout.
      constexpr u64 kDisc = 4;
      const TypeLayout area = enum_payload_area(state, idx, width);
      const u64 align = area.align > kDisc ? area.align : kDisc;
      return {align_up(kDisc, area.align) + area.size, align};
    }
  }
  UNREACHABLE();
}

// Structural Copy query over raw state, shared by Storage and passes
// that read through a builder before build() (see lower). Cycle-free
// input required (see Storage::is_copy_type).
inline bool is_copy_type(const StorageState& state, TypeIdx idx) {
  const TypeNode& node = state.types[idx];
  switch (node.tag) {
    case TypeTag::MutRef: return false;
    case TypeTag::Error: return true;
    case TypeTag::Ref:
    case TypeTag::Void:
    case TypeTag::Never:
    case TypeTag::I1:
    case TypeTag::I8:
    case TypeTag::I16:
    case TypeTag::I32:
    case TypeTag::I64:
    case TypeTag::U8:
    case TypeTag::U16:
    case TypeTag::U32:
    case TypeTag::U64:
    case TypeTag::F32:
    case TypeTag::F64:
    case TypeTag::Str:
    case TypeTag::Ptr:
    case TypeTag::Function: return true;
    case TypeTag::Struct: {
      const StructType& struct_type = state.struct_types[node.as_struct()];
      for (TypeIdx field : struct_type.fields) {
        if (!is_copy_type(state, field)) {
          return false;
        }
      }
      return true;
    }
    case TypeTag::Array:
      return is_copy_type(state, state.array_types[node.as_array()].element);
    case TypeTag::Enum: {
      const EnumType& enum_type = state.enum_types[node.as_enum()];
      for (EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = EnumVariantTypeIdx(vidx.idx + 1)) {
        const EnumVariantType& variant = state.enum_variant_types[vidx];
        for (TypeIdx field : variant.fields) {
          if (!is_copy_type(state, field)) {
            return false;
          }
        }
      }
      return true;
    }
    case TypeTag::Tuple: {
      const TupleType& tuple = state.tuple_types[node.as_tuple()];
      for (TypeIdx element : tuple.elements) {
        if (!is_copy_type(state, element)) {
          return false;
        }
      }
      return true;
    }
    default: UNREACHABLE();
  }
}

class Storage {
 public:
  explicit Storage(StorageState&& state) : state_(std::move(state)) {}
  ~Storage() = default;

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  Storage(Storage&&) noexcept = default;
  Storage& operator=(Storage&&) noexcept = default;

  const StorageState& state() const { return state_; }

  // Moves the state out for phase handoff (e.g. lowering reseeds its
  // builder from checked storage). The Storage must not be used after.
  StorageState take_state() && { return std::move(state_); }

  const StorageState::Functions& functions() const { return state_.functions; }
  const StorageState::Blocks& blocks() const { return state_.blocks; }
  const StorageState::BlockParams& block_params() const {
    return state_.block_params;
  }
  const StorageState::Instructions& instrs() const { return state_.instrs; }
  const StorageState::Immutables& immutables() const {
    return state_.immutables;
  }
  const StorageState::Registers& registers() const { return state_.registers; }
  const StorageState::ExternalFunctions& external_functions() const {
    return state_.external_functions;
  }
  const StorageState::Operands& operands() const { return state_.operands; }
  const StorageState::Types& types() const { return state_.types; }
  const StorageState::StructTypes& struct_types() const {
    return state_.struct_types;
  }
  const StorageState::ArrayTypes& array_types() const {
    return state_.array_types;
  }
  const StorageState::EnumTypes& enum_types() const {
    return state_.enum_types;
  }
  const StorageState::EnumVariantTypes& enum_variant_types() const {
    return state_.enum_variant_types;
  }
  const StorageState::RefTypes& ref_types() const { return state_.ref_types; }
  const StorageState::TupleTypes& tuple_types() const {
    return state_.tuple_types;
  }

  // Copy-ability of a fully interned type: primitives (except MutRef),
  // shared references, units, and structural types whose every part is
  // Copy. Must only run on cycle-free storage; check_package validates
  // uninhabited value cycles before anyone queries. User-defined
  // destructors force move-only once drop syntax lands (no syntax
  // exists yet, so no check is needed here).
  bool is_copy_type(TypeIdx idx) const { return ir::is_copy_type(state_, idx); }

  // Size and alignment of `idx` on `width`. The emitter asserts against
  // this for the types it builds, so a layout rule that disagrees with
  // LLVM is caught where it is introduced.
  TypeLayout layout_of(TypeIdx idx, PointerWidth width) const {
    return ir::type_layout(state_, idx, width);
  }

 private:
  StorageState state_;
};

}  // namespace ir
