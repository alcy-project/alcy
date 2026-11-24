// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
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
#include "ir/verifier.h"

namespace ir {

// What appending a storage's tables moved every reference by: a
// function read from the appended half names its table's base plus the
// index it had there. The two halves share one index space then, and a
// name the other half holds was copied into this build's interner
struct StorageBases {
  u32 types = 0;
  u32 struct_types = 0;
  u32 array_types = 0;
  u32 slice_types = 0;
  u32 enum_types = 0;
  u32 enum_variant_types = 0;
  u32 ref_types = 0;
  u32 tuple_types = 0;
  u32 func_types = 0;
  u32 const_types = 0;
  u32 registers = 0;
  u32 immutables = 0;
  u32 operands = 0;
  u32 instructions = 0;
  u32 blocks = 0;
  u32 block_params = 0;
  u32 functions = 0;
  u32 external_functions = 0;
  u32 parametric_callees = 0;
  // Where each function of the appended half landed. A function a
  // symbol answers for, or one no root reaches, is `U32_MAX` here: the
  // code stays the package's own object's for the first, and the second
  // is not carried at all.
  std::vector<u32> function_map;
  // Where each type of the appended half landed, relative to `types`.
  // A type no root reaches is `U32_MAX`. The per-type tables run
  // parallel to the type table, so this is what a caller extends them
  // by.
  std::vector<u32> type_map;
};

class StorageBuilder {
 public:
  StorageBuilder() { intern_primitives(); }
  explicit StorageBuilder(StorageState&& state) : state_(std::move(state)) {
    index_interned();
  }

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
  StorageBuilder& parametric_callees(
      StorageState::ParametricCallees&& parametric_callees) {
    state_.parametric_callees = std::move(parametric_callees);
    return *this;
  }
  StorageBuilder& operands(StorageState::Operands&& operands) {
    state_.operands = std::move(operands);
    return *this;
  }
  StorageBuilder& parameter_types(StorageState::Types&& parameter_types) {
    state_.types = std::move(parameter_types);
    // The interning tables describe the table that was just replaced.
    index_interned();
    return *this;
  }

  FunctionIdx function(Function function) {
    return state_.functions.emplace_back(function);
  }
  // Resolves a function the appended half holds: a symbol means the
  // code lives in the package's own object and crosses as a call by
  // that name; no symbol means the body is imported.
  using SurfaceResolver =
      std::function<std::optional<str::StringPoolId>(FunctionIdx)>;
  // Copies the part of `other` that `roots` reach after this build's,
  // rewriting each reference through the map its entry moved through:
  // the roots' blocks, instructions, operands, the types they name, the
  // functions they call, and those functions' own closure. An entry no
  // root reaches is not copied, so a dependent holds what its carried
  // bodies reach rather than the other package's whole lowered storage
  //. `map_string` copies a name into this build's
  // interner; a name the other storage's table holds is remapped
  // through it. A function `surface_of` names by symbol keeps no body:
  // a reference to it stays a call by that name.
  StorageBases append_reachable(
      const Storage& other,
      const std::function<str::StringPoolId(str::StringPoolId)>& map_string,
      const SurfaceResolver& surface_of,
      std::span<const FunctionIdx> roots);
  // Backpatches a function's block range: a caller that must see its own
  // index while its body is still being built reserves the row first
  // (the specialization pass does).
  // A function's name is known once the gather interned it: the
  // reservation creates the row, and the handle lands here.
  void set_function_name(FunctionIdx idx, str::StringPoolId name) {
    state_.functions[idx].meta.name = name;
  }
  void set_function_blocks(FunctionIdx idx, BlockIdxRange blocks) {
    DCHECK(idx.idx < state_.functions.size());
    state_.functions[idx].blocks = blocks;
  }
  BlockIdx block(Block block) { return state_.blocks.emplace_back(block); }
  // Backpatches an instruction's operand range, for a rewrite that
  // re-appends the operands: a range must stay contiguous.
  void set_instruction_operands(InstructionIdx idx, OperandIdxRange operands) {
    DCHECK(idx.idx < state_.instrs.size());
    state_.instrs[idx].operands = operands;
  }

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
  const StorageState::SliceTypes& slice_types() const {
    return state_.slice_types;
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
  const StorageState::FuncTypes& func_types() const {
    return state_.func_types;
  }
  const StorageState::ConstTypes& const_types() const {
    return state_.const_types;
  }
  // Raw state for passes that query (never mutate) pre-build tables.
  const StorageState& state() const { return state_; }
  // A point a speculative emission can return to. Const evaluation emits
  // a synthetic function only to read a value back; rolling back to the
  // checkpoint keeps that function out of the published artifact.
  struct Checkpoint {
    usize immutables = 0;
    usize operands = 0;
    usize instrs = 0;
    usize registers = 0;
    usize blocks = 0;
    usize block_params = 0;
    usize functions = 0;
    usize synthetic = 0;
    usize types = 0;
    usize ref_types = 0;
  };
  Checkpoint checkpoint() const {
    return {state_.immutables.size(), state_.operands.size(),
            state_.instrs.size(),     state_.registers.size(),
            state_.blocks.size(),     state_.block_params.size(),
            state_.functions.size(),  state_.synthetic_functions.size(),
            state_.types.size(),      state_.ref_types.size()};
  }
  void rollback(Checkpoint cp) {
    auto pop_to = [](auto& vec, usize size) {
      while (vec.size() > size) {
        vec.pop_back();
      }
    };
    pop_to(state_.immutables, cp.immutables);
    pop_to(state_.operands, cp.operands);
    pop_to(state_.instrs, cp.instrs);
    pop_to(state_.registers, cp.registers);
    pop_to(state_.blocks, cp.blocks);
    pop_to(state_.block_params, cp.block_params);
    pop_to(state_.functions, cp.functions);
    pop_to(state_.synthetic_functions, cp.synthetic);
    pop_to(state_.types, cp.types);
    pop_to(state_.ref_types, cp.ref_types);
    // Every other table is derived from the type table. Rebuild the
    // indexes from what survived rather than trying to pop them: a
    // shape reserved before the checkpoint can have gained payload
    // rows after it, and an intern entry can name a live type by key.
    index_interned();
  }
  // Sizes the tables a caller knows the population of before it fills
  // them. A table grown by doubling halves its own headroom: every
  // block discarded on the way is as large as the one that replaced it.
  void reserve_operands(usize count) { state_.operands.reserve(count); }
  void reserve_instrs(usize count) { state_.instrs.reserve(count); }
  void reserve_registers(usize count) { state_.registers.reserve(count); }
  ExternalFunctionIdx external_function(ExternalFunction external_function) {
    return state_.external_functions.emplace_back(external_function);
  }
  ParametricCalleeIdx parametric_callee(ParametricCallee callee) {
    return state_.parametric_callees.emplace_back(callee);
  }
  // Marks a function as emitted once in its parametric form.
  void add_parametric_function(FunctionIdx function) {
    state_.parametric_functions.push_back(function);
  }
  // Marks a specialization of a staged item, which a reported count
  // excludes.
  void add_prelude_specialization(FunctionIdx function) {
    state_.prelude_specializations.push_back(function);
  }
  // Marks a body the specialization pass synthesized, which a reported
  // count excludes too.
  void add_synthetic_function(FunctionIdx function) {
    state_.synthetic_functions.push_back(function);
  }
  OperandIdx operand(Operand&& operand) {
    return state_.operands.emplace_back(std::move(operand));
  }
  // Returns the pre-interned node for a non-composite tag. O(1), no
  // allocation. Struct, Array, Enum, Never, Tuple, Error, and Const
  // nodes are created via their factories instead.
  TypeIdx primitive(TypeTag tag) const;

  TypeIdx struct_type(str::StringPoolId name,
                      TypeIdxRange fields,
                      TypeIdxRange params) {
    TypeNode node{};
    node.tag = TypeTag::Struct;
    node.data.set(
        state_.struct_types.emplace_back(StructType{.name = name,
                                                    .fields = fields,
                                                    .params = params,
                                                    .hints = {},
                                                    .field_hints = {}}));
    return state_.types.emplace_back(node);
  }

  // The type of a type parameter, by its index in the item it belongs
  // to. Parameters intern like any other type: one parameter is one
  // node, so a body that names it twice names the same type.
  TypeIdx param_type(u32 index);

  // Reserves an empty nominal node that re-entrant references resolve
  // to; fill_struct/fill_enum complete it. Placeholders are valid, so
  // error paths need no cleanup.
  TypeIdx reserve_struct(str::StringPoolId name);

  void fill_struct(TypeIdx idx,
                   TypeIdxRange fields,
                   TypeIdxRange params,
                   TypeLayoutHints hints = {},
                   std::span<const TypeLayoutHints> field_hints = {}) {
    DCHECK(idx.idx < state_.types.size());
    TypeNode& node = state_.types[idx];
    DCHECK(node.tag == TypeTag::Struct);
    StructType& shape = state_.struct_types[node.as_struct()];
    shape.fields = fields;
    shape.params = params;
    shape.hints = hints;
    if (!field_hints.empty()) {
      const u32 head = static_cast<u32>(state_.layout_hints.size());
      for (const TypeLayoutHints& hint : field_hints) {
        state_.layout_hints.emplace_back(hint);
      }
      shape.field_hints = TypeLayoutHintsIdxRange{
          TypeLayoutHintsIdx(head), static_cast<u32>(field_hints.size())};
    }
  }

  // A slice interns by its element alone. The map answers what the
  // factory minted or found itself; a summary merge appends carried
  // types past it, so the table stays the source of truth and a miss
  // falls back to one scan, which repopulates the map.
  TypeIdx slice_type(TypeIdx element);

  TypeIdx array_type(TypeIdx element, u64 count);

  // A compile-time value argument, interned by value so identical
  // arguments share one type index and type equality stays index
  // equality for the generic argument lists that hold them.
  TypeIdx const_type(u64 value);

  // Structural interning: identical pointer shapes share one index, so type
  // equality is index equality. A pointer used to find its shape by scanning
  // every type in the package, and a program names one per borrow it writes or
  // address it takes, so the shape is looked up instead. `Ref`, `MutRef`,
  // `RawPtr`, and `RawMutPtr` of one pointee are four types, so each kind is
  // keyed apart.
  TypeIdx reference_type(TypeIdx pointee, bool is_mut) {
    return pointer_type(is_mut ? TypeTag::MutRef : TypeTag::Ref, pointee);
  }

  // `*T` and `*mut T`. Raw pointers share the reference payload and the
  // interning shape; only their tag differs.
  TypeIdx raw_pointer_type(TypeIdx pointee, bool is_mut) {
    return pointer_type(is_mut ? TypeTag::RawMutPtr : TypeTag::RawPtr, pointee);
  }

  TypeIdx tuple_type(TypeIdxRange elements);

  // Appends a function shape over `params` and `ret`. A signature does
  // not intern here: callers pass copies so the shape's ranges outlive
  // their resolution, and two signatures over one shape are compared
  // structurally by the analyzer, not by this index.
  TypeIdx func_type(TypeIdxRange params, TypeIdx ret);

  TypeIdx never_type();

  TypeIdx error_type();

  EnumVariantTypeIdx enum_variant(str::StringPoolId name, TypeIdxRange fields) {
    return state_.enum_variant_types.emplace_back(
        EnumVariantType{.name = name, .fields = fields});
  }

  // Reserves a nominal node for recursive types; see reserve_struct.
  TypeIdx reserve_enum(str::StringPoolId name);

  void fill_enum(TypeIdx idx,
                 EnumVariantTypeIdxRange variants,
                 TypeIdxRange params,
                 TypeLayoutHints hints = {}) {
    DCHECK(idx.idx < state_.types.size());
    TypeNode& node = state_.types[idx];
    DCHECK(node.tag == TypeTag::Enum);
    EnumType& shape = state_.enum_types[node.as_enum()];
    shape.variants = variants;
    shape.params = params;
    shape.hints = hints;
  }

  TypeIdx enum_type(str::StringPoolId name,
                    EnumVariantTypeIdxRange variants,
                    TypeIdxRange params) {
    TypeNode node{};
    node.tag = TypeTag::Enum;
    node.data.set(state_.enum_types.emplace_back(EnumType{
        .name = name, .variants = variants, .params = params, .hints = {}}));
    return state_.types.emplace_back(node);
  }

  // Appends a copy of an existing type entry, so a field list can
  // reference the same type twice; a range holds each element once.
  TypeIdx ref_type(TypeIdx idx) {
    DCHECK(idx.idx < state_.types.size());
    return state_.types.emplace_back(state_.types[idx]);
  }

  // Hands the tables to a pass that renumbers them wholesale; the
  // builder is empty afterwards and has to be rebuilt from the result.
  StorageState take_state() && { return std::move(state_); }

  // Verifies the built storage and returns it as proof-carrying
  // VerifiedStorage. Invalid builder output becomes a structured
  // error instead of corrupt IR downstream. When verification fails and
  // `failed_out` is given, the unverified tables move there so a reader
  // that only needs type display can still use them.
  base::Result<VerifiedStorage, VerificationError> build(
      StorageState* failed_out = nullptr) &&;

 private:
  TypeIdx pointer_type(TypeTag tag, TypeIdx pointee);

  // Pre-interns every non-composite tag so primitive() resolves without a
  // table lookup. Only the default constructor does this; a builder created
  // from an existing state assumes its table is already populated.
  void intern_primitives();

  // A tuple's elements as one key. The count leads so that a prefix cannot
  // collide with the longer shape by construction, and the elements are
  // combined in order; two shapes that still collide share a bucket and the
  // comparison above tells them apart.
  static u64 elements_key(TypeIdxRange elements);

  // The interning table of a pointer kind, or null for every other tag.
  std::unordered_map<u32, TypeIdx>* pointer_table(TypeTag tag);

  // Rebuilds every index derived from the type table. The scan is the
  // shape lookup used to make, so the first node of a shape is the one a
  // later request interns to. Rollback calls this after dropping the
  // speculative types; import calls it after adopting a type table.
  void index_interned();

  StorageState state_;

  // The two tags that carry no payload, found by looking for a tag they do
  // not have. Both are asked for on paths the type checker takes often, so
  // each remembers where it landed; a type is never retagged, so the first
  // answer stays the answer.
  TypeIdx never_ = TypeIdx::invalid();
  TypeIdx error_ = TypeIdx::invalid();

  // Interned pointer shapes, by the type they point at: a `Ref`, a `MutRef`, a
  // `*T`, and a `*mut T` of one pointee are four types.
  std::unordered_map<u32, TypeIdx> ref_by_pointee_;
  std::unordered_map<u32, TypeIdx> mut_ref_by_pointee_;
  std::unordered_map<u32, TypeIdx> raw_ptr_by_pointee_;
  std::unordered_map<u32, TypeIdx> raw_mut_ptr_by_pointee_;
  // Tuple shapes by a key over their elements, and const shapes by value.
  // Both were found by scanning every type in the package, and a function
  // value asks for its signature's shape once per call site.
  std::unordered_map<u64, std::vector<TypeIdx>> tuple_by_key_;
  std::unordered_map<u64, TypeIdx> const_by_value_;
  // Parameter shapes by the index their node carries. A parameter type is
  // asked for once per type argument of an item, and it used to be found by
  // scanning every type in the package.
  std::unordered_map<u32, TypeIdx> param_by_index_;
  std::unordered_map<u32, TypeIdx> slice_by_element_;
};

}  // namespace ir
