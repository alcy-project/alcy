// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <utility>

#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace ir {

class StorageBuilder {
 public:
  StorageBuilder() { intern_primitives(); }
  explicit StorageBuilder(StorageState&& state) : state_(std::move(state)) {}

  ~StorageBuilder() = default;

  StorageBuilder(const StorageBuilder&) = delete;
  StorageBuilder& operator=(const StorageBuilder&) = delete;

  StorageBuilder(StorageBuilder&&) noexcept = default;
  StorageBuilder& operator=(StorageBuilder&&) noexcept = default;

  StorageBuilder& functions(StorageState::Functions&& functions) {
    state_.functions = std::move(functions);
    return *this;
  }
  StorageBuilder& blocks(StorageState::Blocks&& blocks) {
    state_.blocks = std::move(blocks);
    return *this;
  }
  StorageBuilder& block_params(StorageState::BlockParams&& block_params) {
    state_.block_params = std::move(block_params);
    return *this;
  }
  StorageBuilder& instrs(StorageState::Instructions&& instrs) {
    state_.instrs = std::move(instrs);
    return *this;
  }
  StorageBuilder& immutables(StorageState::Immutables&& immutables) {
    state_.immutables = std::move(immutables);
    return *this;
  }
  StorageBuilder& registers(StorageState::Registers&& registers) {
    state_.registers = std::move(registers);
    return *this;
  }
  StorageBuilder& external_functions(
      StorageState::ExternalFunctions&& external_functions) {
    state_.external_functions = std::move(external_functions);
    return *this;
  }
  StorageBuilder& operands(StorageState::Operands&& operands) {
    state_.operands = std::move(operands);
    return *this;
  }
  StorageBuilder& parameter_types(StorageState::Types&& parameter_types) {
    state_.types = std::move(parameter_types);
    return *this;
  }

  FunctionIdx function(Function function) {
    return state_.functions.emplace_back(function);
  }
  BlockIdx block(Block block) { return state_.blocks.emplace_back(block); }

  // Backpatches a block's instruction range. Branch targets must exist before
  // the branching instruction can reference them, while LLVM requires the
  // entry block first in vector order; declare the entry block empty, build
  // the targets, then patch the entry.
  void set_block_instrs(BlockIdx idx, InstructionIdxRange instrs) {
    DCHECK(idx.idx < state_.blocks.size());
    state_.blocks[idx].instrs = instrs;
  }

  void set_block_params(BlockIdx idx, BlockParamIdxRange params) {
    DCHECK(idx.idx < state_.blocks.size());
    state_.blocks[idx].block_params = params;
  }
  BlockParamIdx block_param(BlockParam block_param) {
    return state_.block_params.emplace_back(block_param);
  }
  InstructionIdx instr(Instruction instr) {
    return state_.instrs.emplace_back(instr);
  }
  ImmutableIdx immutable(Immutable immutable) {
    return state_.immutables.emplace_back(immutable);
  }
  RegisterIdx reg(Register reg) { return state_.registers.emplace_back(reg); }
  // Const readers for analyses running before build(): body checking
  // resolves annotations (interning new types) while inspecting the
  // table, so it reads through the builder instead of a Storage.
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
  // Raw state for passes that query (never mutate) pre-build tables.
  const StorageState& state() const { return state_; }
  ExternalFunctionIdx external_function(ExternalFunction external_function) {
    return state_.external_functions.emplace_back(external_function);
  }
  OperandIdx operand(Operand&& operand) {
    return state_.operands.emplace_back(std::move(operand));
  }
  TypeIdx type(TypeTag tag) {
    DCHECK(tag != TypeTag::Struct && tag != TypeTag::Array &&
           tag != TypeTag::Enum && tag != TypeTag::Never &&
           tag != TypeTag::Tuple && tag != TypeTag::Error);
    return primitive(tag);
  }

  // Returns the pre-interned node for a non-composite tag. O(1), no
  // allocation. Struct, Array, Enum, Never, Tuple, and Error nodes are
  // created via their factories instead.
  TypeIdx primitive(TypeTag tag) const {
    DCHECK(tag != TypeTag::Struct && tag != TypeTag::Array &&
           tag != TypeTag::Enum && tag != TypeTag::Never &&
           tag != TypeTag::Tuple && tag != TypeTag::Error);
    return primitive_idx(tag);
  }

  TypeIdx struct_type(str::StringPoolId name,
                      TypeIdxRange fields,
                      TypeIdxRange params) {
    TypeNode node{};
    node.tag = TypeTag::Struct;
    node.data.set(state_.struct_types.emplace_back(
        StructType{.name = name, .fields = fields, .params = params}));
    return state_.types.emplace_back(node);
  }

  // Reserves a nominal node for recursive types and returns its index
  // immediately with empty contents; fill_struct/fill_enum complete it.
  // Re-entrant references resolve to the reserved index. Placeholders
  // are valid empty nominals, so error paths need no cleanup; the
  // analyzer rejects uninhabited value-only cycles separately.
  TypeIdx reserve_struct(str::StringPoolId name) {
    TypeNode node{};
    node.tag = TypeTag::Struct;
    node.data.set(state_.struct_types.emplace_back(StructType{
        .name = name, .fields = {TypeIdx(0), 0}, .params = {TypeIdx(0), 0}}));
    return state_.types.emplace_back(node);
  }

  void fill_struct(TypeIdx idx, TypeIdxRange fields, TypeIdxRange params) {
    DCHECK(idx.idx < state_.types.size());
    TypeNode& node = state_.types[idx];
    DCHECK(node.tag == TypeTag::Struct);
    state_.struct_types[node.as_struct()].fields = fields;
    state_.struct_types[node.as_struct()].params = params;
  }

  TypeIdx array_type(TypeIdx element, u64 count) {
    for (TypeIdx idx(kPrimitiveTypeCount + 1); idx.idx < state_.types.size();
         ++idx) {
      const TypeNode& node = state_.types[idx];
      if (node.tag != TypeTag::Array) {
        continue;
      }
      const ArrayTypeIdx aidx = node.data.get<ArrayTypeIdx>();
      if (aidx.idx < state_.array_types.size() &&
          state_.array_types[aidx].element.idx == element.idx &&
          state_.array_types[aidx].count == count) {
        return idx;
      }
    }
    TypeNode node{};
    node.tag = TypeTag::Array;
    node.data.set(state_.array_types.emplace_back(
        ArrayType{.element = element, .count = count}));
    return state_.types.emplace_back(node);
  }

  // Structural interning: identical reference shapes share one index, so
  // type equality is index equality. Linear scans are fine at MVP scale;
  // hash tables arrive if measurement demands. Scans start past the
  // pre-interned block, whose placeholder Ref/MutRef payloads must
  // never match.
  TypeIdx reference_type(TypeIdx pointee, bool is_mut) {
    const TypeTag tag = is_mut ? TypeTag::MutRef : TypeTag::Ref;
    for (TypeIdx idx(kPrimitiveTypeCount + 1); idx.idx < state_.types.size();
         ++idx) {
      const TypeNode& node = state_.types[idx];
      if (node.tag != tag) {
        continue;
      }
      const RefTypeIdx ridx = node.data.get<RefTypeIdx>();
      if (ridx.idx < state_.ref_types.size() &&
          state_.ref_types[ridx].pointee.idx == pointee.idx) {
        return idx;
      }
    }
    TypeNode node{};
    node.tag = tag;
    node.data.set(state_.ref_types.emplace_back(RefType{.pointee = pointee}));
    return state_.types.emplace_back(node);
  }

  TypeIdx tuple_type(TypeIdxRange elements) {
    for (TypeIdx idx(kPrimitiveTypeCount + 1); idx.idx < state_.types.size();
         ++idx) {
      const TypeNode& node = state_.types[idx];
      if (node.tag != TypeTag::Tuple) {
        continue;
      }
      const TupleTypeIdx tidx = node.data.get<TupleTypeIdx>();
      if (tidx.idx >= state_.tuple_types.size()) {
        continue;
      }
      const TupleType& tuple = state_.tuple_types[tidx];
      if (tuple.elements.size() != elements.size()) {
        continue;
      }
      bool match = true;
      for (u32 i = 0; match && i < elements.size(); ++i) {
        match = tuple.elements[i].idx == elements[i].idx;
      }
      if (match) {
        return idx;
      }
    }
    TypeNode node{};
    node.tag = TypeTag::Tuple;
    // Elements are already interned; only the shape node is new.
    TupleType tuple;
    tuple.elements = elements;
    node.data.set(state_.tuple_types.emplace_back(tuple));
    return state_.types.emplace_back(node);
  }

  TypeIdx never_type() {
    for (TypeIdx idx(kPrimitiveTypeCount + 1); idx.idx < state_.types.size();
         ++idx) {
      if (state_.types[idx].tag == TypeTag::Never) {
        return idx;
      }
    }
    TypeNode node{};
    node.tag = TypeTag::Never;
    return state_.types.emplace_back(node);
  }

  TypeIdx error_type() {
    for (TypeIdx idx(kPrimitiveTypeCount + 1); idx.idx < state_.types.size();
         ++idx) {
      if (state_.types[idx].tag == TypeTag::Error) {
        return idx;
      }
    }
    TypeNode node{};
    node.tag = TypeTag::Error;
    return state_.types.emplace_back(node);
  }

  EnumVariantTypeIdx enum_variant(str::StringPoolId name, TypeIdxRange fields) {
    return state_.enum_variant_types.emplace_back(
        EnumVariantType{.name = name, .fields = fields});
  }

  // Reserves a nominal node for recursive types; see reserve_struct.
  TypeIdx reserve_enum(str::StringPoolId name) {
    TypeNode node{};
    node.tag = TypeTag::Enum;
    node.data.set(state_.enum_types.emplace_back(
        EnumType{.name = name,
                 .variants = {EnumVariantTypeIdx(0), 0},
                 .params = {TypeIdx(0), 0}}));
    return state_.types.emplace_back(node);
  }

  void fill_enum(TypeIdx idx,
                 EnumVariantTypeIdxRange variants,
                 TypeIdxRange params) {
    DCHECK(idx.idx < state_.types.size());
    TypeNode& node = state_.types[idx];
    DCHECK(node.tag == TypeTag::Enum);
    state_.enum_types[node.as_enum()].variants = variants;
    state_.enum_types[node.as_enum()].params = params;
  }

  TypeIdx enum_type(str::StringPoolId name,
                    EnumVariantTypeIdxRange variants,
                    TypeIdxRange params) {
    TypeNode node{};
    node.tag = TypeTag::Enum;
    node.data.set(state_.enum_types.emplace_back(
        EnumType{.name = name, .variants = variants, .params = params}));
    return state_.types.emplace_back(node);
  }

  // Appends a copy of an existing type entry and returns the new index.
  // Useful for building field lists that reference the same type twice.
  TypeIdx ref_type(TypeIdx idx) {
    DCHECK(idx.idx < state_.types.size());
    return state_.types.emplace_back(state_.types[idx]);
  }

  Storage build() && { return Storage(std::move(state_)); }

  std::unique_ptr<Storage> build_unique() && {
    return std::make_unique<Storage>(std::move(*this).build());
  }

 private:
  // Pre-interns every non-composite tag so primitive() resolves without a
  // table lookup. Only the default constructor does this; a builder created
  // from an existing state assumes its table is already populated.
  void intern_primitives() {
    for (u32 i = 0; i < kPrimitiveTypeCount; ++i) {
      TypeNode node{};
      node.tag = static_cast<TypeTag>(i);
      state_.types.emplace_back(node);
    }
    TypeNode func{};
    func.tag = TypeTag::Function;
    state_.types.emplace_back(func);
  }

  StorageState state_;
};

}  // namespace ir
