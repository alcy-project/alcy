// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "fpag/base/vec.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/parametric.h"
#include "ir/register.h"
#include "ir/type.h"

namespace ir {

struct StorageState {
  // base::Vec, not an arena: index stability, random access, and DOD
  // iteration all favor a dense table.

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
  using ParametricCallees =
      base::Vec<ParametricCallee, ParametricCalleeIdx, Alloc<ParametricCallee>>;
  using Operands = base::Vec<Operand, OperandIdx, Alloc<Operand>>;
  using Types = base::Vec<TypeNode, TypeIdx, Alloc<TypeNode>>;
  using StructTypes = base::Vec<StructType, StructTypeIdx, Alloc<StructType>>;
  using ArrayTypes = base::Vec<ArrayType, ArrayTypeIdx, Alloc<ArrayType>>;
  using SliceTypes = base::Vec<SliceType, SliceTypeIdx, Alloc<SliceType>>;
  using EnumTypes = base::Vec<EnumType, EnumTypeIdx, Alloc<EnumType>>;
  using EnumVariantTypes =
      base::Vec<EnumVariantType, EnumVariantTypeIdx, Alloc<EnumVariantType>>;
  using RefTypes = base::Vec<RefType, RefTypeIdx, Alloc<RefType>>;
  using TupleTypes = base::Vec<TupleType, TupleTypeIdx, Alloc<TupleType>>;
  using FuncTypes = base::Vec<FuncType, FuncTypeIdx, Alloc<FuncType>>;
  using ConstTypes = base::Vec<ConstType, ConstTypeIdx, Alloc<ConstType>>;
  using LayoutHints =
      base::Vec<TypeLayoutHints, TypeLayoutHintsIdx, Alloc<TypeLayoutHints>>;

  Functions functions;
  Blocks blocks;
  BlockParams block_params;
  Instructions instrs;
  Immutables immutables;
  Registers registers;
  ExternalFunctions external_functions;
  ParametricCallees parametric_callees;
  // The functions emitted once in their parametric form: the
  // specialization pass clones them per instantiation, and no backend
  // receives them. Kept beside the table rather than in a
  // range because their indexes are not consecutive.
  std::vector<FunctionIdx> parametric_functions;
  // The specializations of staged items: clones no source function
  // stands for, which a build's reported count excludes. Their indexes
  // are not consecutive either.
  std::vector<FunctionIdx> prelude_specializations;
  // The bodies the specialization pass synthesized (`display$`,
  // `drop$`): compiler-made helpers a reported count excludes too.
  std::vector<FunctionIdx> synthetic_functions;
  Operands operands;
  Types types;
  StructTypes struct_types;
  ArrayTypes array_types;
  SliceTypes slice_types;
  EnumTypes enum_types;
  EnumVariantTypes enum_variant_types;
  RefTypes ref_types;
  TupleTypes tuple_types;
  FuncTypes func_types;
  ConstTypes const_types;
  LayoutHints layout_hints;
  // Answers `type_names_param` keeps per type: 0 unanswered, 1 no, 2
  // yes, 3 on the walk's own stack. Type nodes never change once
  // interned, so an answer holds for the state's lifetime.
  mutable std::vector<u8> names_param_memo;
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

TypeLayout type_layout(const StorageState& state,
                       TypeIdx idx,
                       PointerWidth width);
TypeLayout fields_layout(const StorageState& state,
                         TypeIdxRange fields,
                         PointerWidth width,
                         const TypeLayoutHints& hints = {},
                         const TypeLayoutHints* field_hints = nullptr,
                         u32 field_hint_count = 0);
// The niche an option-shaped enum's discriminant lives in :
// exactly two variants, one carrying nothing and one carrying a single
// reference. The reference's unused null pattern holds the fieldless
// variant, so a value of the enum is the payload itself. A `bool`
// payload carries a spare pattern too; it lands here once its storage
// type can hold one.
struct EnumNiche {
  bool has = false;
  u32 carrying = 0;
  u32 fieldless = 0;
  TypeIdx payload = TypeIdx(0);
  // The type the slot holds: the payload itself, or the byte a `bool`
  // payload's third state needs.
  TypeIdx slot = TypeIdx(0);
  // The byte that spells the fieldless variant when the slot is a byte;
  // zero when the payload is a reference and null does the work.
  u8 sentinel = 0;
};

// The enum type a place names: a place read through a reference
// arrives labelled with the reference, and the enum is what it points
// at. Every other label is its own type.
TypeIdx enum_place_type(const StorageState& state, TypeIdx idx);

EnumNiche enum_niche(const StorageState& state, TypeIdx idx);

TypeLayout enum_payload_area(const StorageState& state,
                             TypeIdx idx,
                             PointerWidth width);

// The per-field layout overrides of a struct, as a span into the
// storage's dense hint table. An empty span means the compiler's natural
// layout. The range is validated by the verifier before any IR is built.
std::pair<const TypeLayoutHints*, u32> field_hints_of(const StorageState& state,
                                                      const StructType& shape);

// Layout of a field list: each field at the next offset its effective
// alignment allows, where a field's layout setting can raise or cap it.
// An empty list has no size, which is what an uninhabited payload needs.
TypeLayout fields_layout(const StorageState& state,
                         TypeIdxRange fields,
                         PointerWidth width,
                         const TypeLayoutHints& hints,
                         const TypeLayoutHints* field_hints,
                         u32 field_hint_count);

// Byte offset of field `index` in a field list laid out by
// fields_layout. Callers pass the same field layout settings so an offset
// and the size that reserved it cannot disagree.
u64 field_offset(const StorageState& state,
                 TypeIdxRange fields,
                 u32 index,
                 PointerWidth width,
                 const TypeLayoutHints* hints = nullptr,
                 u32 hint_count = 0);

// The discriminant's byte count: the smallest whole byte count that
// names every variant, unless the declaration fixed one.
u8 enum_tag_width(const StorageState& state, TypeIdx idx);

// An enum needs no discriminant when it has no values, or when its
// single variant carries nothing but zero-sized payloads.
bool enum_is_zst(const StorageState& state, TypeIdx idx);

// Layout of an enum's payload area: room for the widest variant
// payload, aligned for the strictest payload field. The size is a whole
// number of carriers so the emitted array has exactly this layout and a
// field's offset within the area is the same number the lowerer and the
// emitter both read.
TypeLayout enum_payload_area(const StorageState& state,
                             TypeIdx idx,
                             PointerWidth width);

TypeLayout type_layout(const StorageState& state,
                       TypeIdx idx,
                       PointerWidth width);

// A walk's record of the shapes it is inside. A structural walk follows
// the fields of a shape, so a shape that holds itself would follow
// itself to the end of the stack. The cycle checks refuse such a shape
// (EC007) once the modules are processed, and they throw this answer
// away; reaching a walk first must not end the compiler.
class InsideScope {
 public:
  InsideScope(std::vector<u32>& inside, u32 idx) : inside_(inside) {
    inside_.push_back(idx);
  }
  ~InsideScope() { inside_.pop_back(); }
  InsideScope(const InsideScope&) = delete;
  InsideScope& operator=(const InsideScope&) = delete;

 private:
  std::vector<u32>& inside_;
};

// Structural movability over raw state: a value is movable
// unless it holds an anchor. `not_movable` marks the types a
// declaration makes unmovable - the anchor itself - and the walk is
// structural, so a struct, enum, tuple, or array holding one is
// unmovable too. `origin` resolves a storage copy to the declaration
// it came from, because a field slot holds a copy. `inside` names the
// shapes the walk is already inside; one that holds itself answers as
// movable, because the cycle checks are what speak on it.
template <class Origin>
inline bool is_movable_type(const StorageState& state,
                            TypeIdx idx,
                            std::span<const u8> not_movable,
                            const Origin& origin,
                            std::vector<u32>& inside);

template <class Origin>
inline bool is_movable_type(const StorageState& state,
                            TypeIdx idx,
                            std::span<const u8> not_movable,
                            const Origin& origin) {
  std::vector<u32> inside;
  return is_movable_type(state, idx, not_movable, origin, inside);
}

template <class Origin>
inline bool is_movable_type(const StorageState& state,
                            TypeIdx idx,
                            std::span<const u8> not_movable,
                            const Origin& origin,
                            std::vector<u32>& inside) {
  idx = origin(idx);
  if (idx.idx < not_movable.size() && not_movable[idx.idx] != 0) {
    return false;
  }
  if (std::find(inside.begin(), inside.end(), idx.idx) != inside.end()) {
    return true;
  }
  const InsideScope record(inside, idx.idx);
  const TypeNode& node = state.types[idx];
  switch (node.tag) {
    case TypeTag::Error: return true;
    case TypeTag::Const: UNREACHABLE();
    // A type parameter is Movable: a type argument is, and a body that
    // has not been specialized can assume it.
    case TypeTag::Param: return true;
    case TypeTag::Struct: {
      const StructType& struct_type = state.struct_types[node.as_struct()];
      for (TypeIdx field : struct_type.fields) {
        if (!is_movable_type(state, field, not_movable, origin, inside)) {
          return false;
        }
      }
      return true;
    }
    case TypeTag::Array:
      return is_movable_type(state, state.array_types[node.as_array()].element,
                             not_movable, origin, inside);
    case TypeTag::Slice:
      return is_movable_type(state, state.slice_types[node.as_slice()].element,
                             not_movable, origin, inside);
    case TypeTag::Enum: {
      const EnumType& enum_type = state.enum_types[node.as_enum()];
      for (EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = EnumVariantTypeIdx(vidx.idx + 1)) {
        const EnumVariantType& variant = state.enum_variant_types[vidx];
        for (TypeIdx field : variant.fields) {
          if (!is_movable_type(state, field, not_movable, origin, inside)) {
            return false;
          }
        }
      }
      return true;
    }
    case TypeTag::Tuple: {
      const TupleType& tuple = state.tuple_types[node.as_tuple()];
      for (TypeIdx element : tuple.elements) {
        if (!is_movable_type(state, element, not_movable, origin, inside)) {
          return false;
        }
      }
      return true;
    }
    default: return true;
  }
}

// Structural Copy query over raw state, shared by Storage and passes
// that read through a builder before build() (see lowering). Cycle-free
// input required (see Storage::is_copy_type). `has_drop` marks the
// types a declaration gives a destructor: such a type is move-only,
// whatever its fields are. `inside` names the shapes the walk is
// already inside; one that holds itself answers as Copy, because the
// cycle checks are what speak on it.
bool is_copy_type(const StorageState& state,
                  TypeIdx idx,
                  std::span<const u8> has_drop,
                  std::span<const u8> not_movable,
                  std::vector<u32>& inside);

bool is_copy_type(const StorageState& state,
                  TypeIdx idx,
                  std::span<const u8> has_drop,
                  std::span<const u8> not_movable = {});

bool is_copy_type(const StorageState& state,
                  TypeIdx idx,
                  std::span<const u8> has_drop,
                  std::span<const u8> not_movable,
                  std::vector<u32>& inside);

// The structural question alone, for the callers that have no
// destructor table in hand.
bool is_copy_type(const StorageState& state, TypeIdx idx);

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
  const StorageState::ParametricCallees& parametric_callees() const {
    return state_.parametric_callees;
  }
  const std::vector<FunctionIdx>& parametric_functions() const {
    return state_.parametric_functions;
  }
  // Whether a function is emitted once in its parametric form. A pass
  // that asks about one body asks here; a pass that scans every
  // function builds its membership set from the list once.
  [[nodiscard]] bool is_parametric(FunctionIdx function) const {
    const std::vector<FunctionIdx>& functions = parametric_functions();
    return std::find(functions.begin(), functions.end(), function) !=
           functions.end();
  }
  const std::vector<FunctionIdx>& prelude_specializations() const {
    return state_.prelude_specializations;
  }
  const std::vector<FunctionIdx>& synthetic_functions() const {
    return state_.synthetic_functions;
  }
  const StorageState::Operands& operands() const { return state_.operands; }
  const StorageState::Types& types() const { return state_.types; }
  const StorageState::StructTypes& struct_types() const {
    return state_.struct_types;
  }
  const StorageState::LayoutHints& layout_hints() const {
    return state_.layout_hints;
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

  // Copy-ability of a fully interned type. check_package validates
  // uninhabited value cycles, so a storage that passed the checks has
  // none; the walk answers rather than following one, because a
  // declaration those checks refuse is queried before they run.
  // User-defined destructors force move-only once drop syntax lands.
  bool is_copy_type(TypeIdx idx) const { return ir::is_copy_type(state_, idx); }

  TypeLayout layout_of(TypeIdx idx, PointerWidth width) const {
    return ir::type_layout(state_, idx, width);
  }

 private:
  StorageState state_;
};

// Storage that passed structural verification at build time. The only
// production path to a Storage is StorageBuilder::build, so holding
// this type proves verify_storage ran; consumers that require valid
// IR (lowering output, the emitter) take it instead of re-verifying.
class VerifiedStorage {
 public:
  ~VerifiedStorage() = default;

  VerifiedStorage(const VerifiedStorage&) = delete;
  VerifiedStorage& operator=(const VerifiedStorage&) = delete;

  VerifiedStorage(VerifiedStorage&&) noexcept = default;
  VerifiedStorage& operator=(VerifiedStorage&&) noexcept = default;

  const Storage& operator*() const { return storage_; }
  const Storage* operator->() const { return &storage_; }

  // Moves the verified storage out for phase handoff. The proof is
  // consumed: downstream code holds a plain Storage again.
  Storage unwrap() && { return std::move(storage_); }

 private:
  friend class StorageBuilder;
  explicit VerifiedStorage(Storage storage) : storage_(std::move(storage)) {}

  Storage storage_;
};

}  // namespace ir
