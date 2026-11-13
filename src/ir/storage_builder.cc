// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/storage_builder.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debug/dcheck.h"
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
#include "ir/storage.h"
#include "ir/type.h"

namespace ir {

namespace {

// One reference into an appended table: invalid stays invalid.
template <typename Idx>
Idx mapped(Idx idx, const std::vector<u32>& map, u32 base) {
  if (!idx.is_valid()) {
    return idx;
  }
  DCHECK(idx.idx < map.size());
  DCHECK(map[idx.idx] != U32_MAX);
  return Idx(map[idx.idx] + base);
}

// A range whose entries all moved: the copy assigns new indexes in
// source order, so a range a reachable entry holds stays consecutive
// and the head's new index carries the whole range.
template <typename T>
base::IdxRange<T> mapped_range(base::IdxRange<T> range,
                               const std::vector<u32>& map,
                               u32 base) {
  if (range.size() == 0) {
    return range;
  }
  DCHECK(range.head().idx < map.size());
  DCHECK(map[range.head().idx] != U32_MAX);
  DCHECK_EQ(map[range.head().idx + range.size() - 1],
            map[range.head().idx] + range.size() - 1);
  return base::IdxRange<T>(T(map[range.head().idx] + base), range.size());
}

// The relative index each marked entry lands on, assigned in source
// order.
std::vector<u32> index_map(const std::vector<bool>& marked) {
  std::vector<u32> map(marked.size(), U32_MAX);
  u32 next = 0;
  for (usize i = 0; i < marked.size(); ++i) {
    if (marked[i]) {
      map[i] = next++;
    }
  }
  return map;
}

// The entries of a storage one or more roots reach: a reference from a
// marked entry marks what it names, and the walk repeats until nothing
// new is marked. The copy that follows moves only these, so a dependent
// holds what its carried bodies reach rather than the library's whole
// lowered storage (ADR-0079).
struct Reached {
  enum class Kind : u8 {
    Type,
    Struct,
    Array,
    Slice,
    Enum,
    EnumVariant,
    Ref,
    Tuple,
    Func,
    Const,
    Register,
    Immutable,
    Operand,
    Instr,
    Block,
    BlockParam,
    Function,
    ParametricCallee,
    External,
    Count,
  };
  static constexpr usize KIND_COUNT = static_cast<usize>(Kind::Count);
  struct Entry {
    Kind kind;
    u32 idx;
  };

  Reached(const StorageState& source,
          const StorageBuilder::SurfaceResolver& surface_of)
      : src(source), surface(surface_of) {
    const auto size = [&](Kind kind, usize count) {
      marks[static_cast<usize>(kind)].resize(count);
    };
    size(Kind::Type, src.types.size());
    size(Kind::Struct, src.struct_types.size());
    size(Kind::Array, src.array_types.size());
    size(Kind::Slice, src.slice_types.size());
    size(Kind::Enum, src.enum_types.size());
    size(Kind::EnumVariant, src.enum_variant_types.size());
    size(Kind::Ref, src.ref_types.size());
    size(Kind::Tuple, src.tuple_types.size());
    size(Kind::Func, src.func_types.size());
    size(Kind::Const, src.const_types.size());
    size(Kind::Register, src.registers.size());
    size(Kind::Immutable, src.immutables.size());
    size(Kind::Operand, src.operands.size());
    size(Kind::Instr, src.instrs.size());
    size(Kind::Block, src.blocks.size());
    size(Kind::BlockParam, src.block_params.size());
    size(Kind::Function, src.functions.size());
    size(Kind::ParametricCallee, src.parametric_callees.size());
    size(Kind::External, src.external_functions.size());
  }

  const StorageState& src;
  const StorageBuilder::SurfaceResolver& surface;
  std::array<std::vector<bool>, KIND_COUNT> marks;
  std::vector<Entry> work;

  [[nodiscard]] const std::vector<bool>& of(Kind kind) const {
    return marks[static_cast<usize>(kind)];
  }

  void mark(Kind kind, u32 idx) {
    std::vector<bool>& set = marks[static_cast<usize>(kind)];
    if (idx >= set.size() || set[idx]) {
      return;
    }
    set[idx] = true;
    work.push_back(Entry{kind, idx});
  }

  // A function a root or a reference names: the carried body joins the
  // copy, while a function a symbol answers for stays the other
  // package's own code, so only its declaration's shape moves.
  void mark_function(FunctionIdx function) {
    if (!function.is_valid() || function.idx >= src.functions.size()) {
      return;
    }
    if (surface(function).has_value()) {
      mark_meta(src.functions[function.idx].meta);
      return;
    }
    mark(Kind::Function, function.idx);
  }

  void mark_type(TypeIdx type) {
    if (!type.is_valid() || type.idx >= src.types.size()) {
      return;
    }
    mark(Kind::Type, type.idx);
  }

  void mark_meta(const FunctionMeta& meta) {
    mark_type(meta.return_type);
    for (const TypeIdx param : meta.param_types) {
      mark_type(param);
    }
    for (const TypeIdx generic : meta.generics) {
      mark_type(generic);
    }
    mark_type(meta.owner);
  }

  void run() {
    while (!work.empty()) {
      const Entry entry = work.back();
      work.pop_back();
      visit(entry);
    }
  }

  void visit(const Entry& entry) {
    switch (entry.kind) {
      case Kind::Type: visit_type(entry.idx); break;
      case Kind::Struct: visit_struct(entry.idx); break;
      case Kind::Array: visit_array(entry.idx); break;
      case Kind::Slice: visit_slice(entry.idx); break;
      case Kind::Enum: visit_enum(entry.idx); break;
      case Kind::EnumVariant: visit_enum_variant(entry.idx); break;
      case Kind::Ref: visit_ref(entry.idx); break;
      case Kind::Tuple: visit_tuple(entry.idx); break;
      case Kind::Func: visit_func(entry.idx); break;
      case Kind::Const: break;
      case Kind::Register: visit_register(entry.idx); break;
      case Kind::Immutable: visit_immutable(entry.idx); break;
      case Kind::Operand: visit_operand(entry.idx); break;
      case Kind::Instr: visit_instr(entry.idx); break;
      case Kind::Block: visit_block(entry.idx); break;
      case Kind::BlockParam: visit_block_param(entry.idx); break;
      case Kind::Function: visit_function(entry.idx); break;
      case Kind::ParametricCallee: visit_parametric_callee(entry.idx); break;
      case Kind::External: visit_external(entry.idx); break;
      case Kind::Count: break;
    }
  }

  void visit_type(u32 idx) {
    const TypeNode& node = src.types[TypeIdx(idx)];
    using T = TypeTag;
    switch (node.tag) {
      case T::Struct: mark(Kind::Struct, node.as_struct().idx); break;
      case T::Array: mark(Kind::Array, node.as_array().idx); break;
      case T::Slice: mark(Kind::Slice, node.as_slice().idx); break;
      case T::Enum: mark(Kind::Enum, node.as_enum().idx); break;
      case T::Ref:
      case T::MutRef:
      case T::RawPtr:
      case T::RawMutPtr: mark(Kind::Ref, node.as_ref().idx); break;
      case T::Tuple: mark(Kind::Tuple, node.as_tuple().idx); break;
      case T::Func: mark(Kind::Func, node.as_func().idx); break;
      case T::Const: mark(Kind::Const, node.as_const().idx); break;
      case T::Void:
      case T::I1:
      case T::I8:
      case T::I16:
      case T::I32:
      case T::I64:
      case T::U8:
      case T::U16:
      case T::U32:
      case T::U64:
      case T::F32:
      case T::F64:
      case T::Str:
      case T::Ptr:
      case T::Function:
      case T::Never:
      case T::Error:
      case T::Param: break;
    }
  }

  void visit_struct(u32 idx) {
    const StructType& shape = src.struct_types[StructTypeIdx(idx)];
    for (const TypeIdx field : shape.fields) {
      mark_type(field);
    }
    for (const TypeIdx param : shape.params) {
      mark_type(param);
    }
  }

  void visit_array(u32 idx) {
    mark_type(src.array_types[ArrayTypeIdx(idx)].element);
  }

  void visit_slice(u32 idx) {
    mark_type(src.slice_types[SliceTypeIdx(idx)].element);
  }

  void visit_enum(u32 idx) {
    const EnumType& shape = src.enum_types[EnumTypeIdx(idx)];
    for (const EnumVariantTypeIdx variant : shape.variants) {
      mark(Kind::EnumVariant, variant.idx);
    }
    for (const TypeIdx param : shape.params) {
      mark_type(param);
    }
  }

  void visit_enum_variant(u32 idx) {
    for (const TypeIdx field :
         src.enum_variant_types[EnumVariantTypeIdx(idx)].fields) {
      mark_type(field);
    }
  }

  void visit_ref(u32 idx) { mark_type(src.ref_types[RefTypeIdx(idx)].pointee); }

  void visit_tuple(u32 idx) {
    for (const TypeIdx element : src.tuple_types[TupleTypeIdx(idx)].elements) {
      mark_type(element);
    }
  }

  void visit_func(u32 idx) {
    const FuncType& shape = src.func_types[FuncTypeIdx(idx)];
    for (const TypeIdx param : shape.params) {
      mark_type(param);
    }
    mark_type(shape.ret);
  }

  void visit_register(u32 idx) {
    mark_type(src.registers[RegisterIdx(idx)].type);
  }

  void visit_immutable(u32 idx) {
    mark_type(src.immutables[ImmutableIdx(idx)].type);
  }

  void visit_operand(u32 idx) {
    const Operand& operand = src.operands[OperandIdx(idx)];
    mark_type(operand.type);
    switch (operand.tag()) {
      case Operand::TAG_OF<RegisterIdx>:
        mark(Kind::Register, operand.as_register().idx);
        break;
      case Operand::TAG_OF<FunctionIdx>:
        mark_function(operand.as_function());
        break;
      case Operand::TAG_OF<BlockIdx>:
        mark(Kind::Block, operand.as_block().idx);
        break;
      case Operand::TAG_OF<ImmutableIdx>:
        mark(Kind::Immutable, operand.as_immutable().idx);
        break;
      case Operand::TAG_OF<ParametricCalleeIdx>:
        mark(Kind::ParametricCallee, operand.as_parametric_callee().idx);
        break;
      case Operand::TAG_OF<ExternalFunctionIdx>:
        mark(Kind::External, operand.as_external_function().idx);
        break;
      default: break;
    }
  }

  void visit_instr(u32 idx) {
    const Instruction& instr = src.instrs[InstructionIdx(idx)];
    if (instr.dst.is_valid()) {
      mark(Kind::Register, instr.dst.idx);
    }
    mark_type(instr.measure);
    for (const OperandIdx operand : instr.operands) {
      mark(Kind::Operand, operand.idx);
    }
  }

  void visit_block(u32 idx) {
    const Block& block = src.blocks[BlockIdx(idx)];
    for (const InstructionIdx instr : block.instrs) {
      mark(Kind::Instr, instr.idx);
    }
    for (const BlockParamIdx param : block.block_params) {
      mark(Kind::BlockParam, param.idx);
    }
  }

  void visit_block_param(u32 idx) {
    const BlockParam& param = src.block_params[BlockParamIdx(idx)];
    mark_type(param.type);
    if (param.reg.is_valid()) {
      mark(Kind::Register, param.reg.idx);
    }
  }

  void visit_function(u32 idx) {
    const Function& function = src.functions[FunctionIdx(idx)];
    mark_meta(function.meta);
    for (const BlockIdx block : function.blocks) {
      mark(Kind::Block, block.idx);
    }
  }

  void visit_parametric_callee(u32 idx) {
    const ParametricCallee& callee =
        src.parametric_callees[ParametricCalleeIdx(idx)];
    if (callee.function.is_valid()) {
      mark_function(callee.function);
    }
    for (const TypeIdx arg : callee.args) {
      mark_type(arg);
    }
  }

  void visit_external(u32 idx) {
    mark_meta(src.external_functions[ExternalFunctionIdx(idx)].meta);
  }
};

}  // namespace

StorageBases StorageBuilder::append_reachable(
    const Storage& other,
    const std::function<str::StringPoolId(str::StringPoolId)>& map_string,
    const SurfaceResolver& surface_of,
    std::span<const FunctionIdx> roots) {
  const StorageState& src = other.state();
  Reached reached(src, surface_of);
  for (const FunctionIdx root : roots) {
    reached.mark_function(root);
  }
  reached.run();

  // Every base is the table's size before the append: an entry read from
  // the appended half names its base plus the index it landed on.
  StorageBases bases;
  bases.types = static_cast<u32>(state_.types.size());
  bases.struct_types = static_cast<u32>(state_.struct_types.size());
  bases.array_types = static_cast<u32>(state_.array_types.size());
  bases.slice_types = static_cast<u32>(state_.slice_types.size());
  bases.enum_types = static_cast<u32>(state_.enum_types.size());
  bases.enum_variant_types = static_cast<u32>(state_.enum_variant_types.size());
  bases.ref_types = static_cast<u32>(state_.ref_types.size());
  bases.tuple_types = static_cast<u32>(state_.tuple_types.size());
  bases.func_types = static_cast<u32>(state_.func_types.size());
  bases.const_types = static_cast<u32>(state_.const_types.size());
  bases.registers = static_cast<u32>(state_.registers.size());
  bases.immutables = static_cast<u32>(state_.immutables.size());
  bases.operands = static_cast<u32>(state_.operands.size());
  bases.instructions = static_cast<u32>(state_.instrs.size());
  bases.blocks = static_cast<u32>(state_.blocks.size());
  bases.block_params = static_cast<u32>(state_.block_params.size());
  bases.functions = static_cast<u32>(state_.functions.size());
  bases.external_functions = static_cast<u32>(state_.external_functions.size());
  bases.parametric_callees = static_cast<u32>(state_.parametric_callees.size());

  // The relative index each reachable entry lands on.
  const std::vector<u32> type_map = index_map(reached.of(Reached::Kind::Type));
  const std::vector<u32> struct_map =
      index_map(reached.of(Reached::Kind::Struct));
  const std::vector<u32> array_map =
      index_map(reached.of(Reached::Kind::Array));
  const std::vector<u32> slice_map =
      index_map(reached.of(Reached::Kind::Slice));
  const std::vector<u32> enum_map = index_map(reached.of(Reached::Kind::Enum));
  const std::vector<u32> variant_map =
      index_map(reached.of(Reached::Kind::EnumVariant));
  const std::vector<u32> ref_map = index_map(reached.of(Reached::Kind::Ref));
  const std::vector<u32> tuple_map =
      index_map(reached.of(Reached::Kind::Tuple));
  const std::vector<u32> func_map = index_map(reached.of(Reached::Kind::Func));
  const std::vector<u32> const_map =
      index_map(reached.of(Reached::Kind::Const));
  const std::vector<u32> register_map =
      index_map(reached.of(Reached::Kind::Register));
  const std::vector<u32> immutable_map =
      index_map(reached.of(Reached::Kind::Immutable));
  const std::vector<u32> operand_map =
      index_map(reached.of(Reached::Kind::Operand));
  const std::vector<u32> instr_map =
      index_map(reached.of(Reached::Kind::Instr));
  const std::vector<u32> block_map =
      index_map(reached.of(Reached::Kind::Block));
  const std::vector<u32> block_param_map =
      index_map(reached.of(Reached::Kind::BlockParam));
  const std::vector<u32> callee_map =
      index_map(reached.of(Reached::Kind::ParametricCallee));
  // Where each carried function lands, absolute: the callers index the
  // function table with it.
  std::vector<u32> function_map(src.functions.size(), U32_MAX);
  {
    u32 next = bases.functions;
    for (usize i = 0; i < src.functions.size(); ++i) {
      if (reached.of(Reached::Kind::Function)[i]) {
        function_map[i] = next++;
      }
    }
  }
  bases.function_map = function_map;
  bases.type_map = type_map;

  const auto type = [&](TypeIdx idx) {
    return mapped(idx, type_map, bases.types);
  };
  const auto type_range = [&](TypeIdxRange range) {
    return mapped_range(range, type_map, bases.types);
  };
  const auto name = [&](str::StringPoolId id) {
    return id == str::INVALID_STRING_POOL_ID ? id : map_string(id);
  };
  // An external a package already declares - the allocator, the panic
  // hook, a libc call - is one entry: a merged reference lands on the
  // existing declaration, which is the name a runtime definition
  // answers, instead of a second declaration LLVM has to rename.
  std::unordered_map<u32, u32> externals_by_name;
  for (u32 i = 0; i < static_cast<u32>(state_.external_functions.size()); ++i) {
    externals_by_name.emplace(state_.external_functions[i].meta.name.offset, i);
  }
  const auto meta_of = [&](const FunctionMeta& meta) {
    return FunctionMeta{.return_type = type(meta.return_type),
                        .param_types = type_range(meta.param_types),
                        .name = name(meta.name),
                        .path = name(meta.path),
                        .kind = meta.kind,
                        .generics = type_range(meta.generics),
                        .owner = type(meta.owner)};
  };
  // The declaration a call to another object's function names: one per
  // symbol, however many references reach it.
  const auto external_for = [&](str::StringPoolId symbol,
                                const FunctionMeta& source) {
    const auto found = externals_by_name.find(symbol.offset);
    if (found != externals_by_name.end()) {
      return ExternalFunctionIdx(found->second);
    }
    FunctionMeta meta = meta_of(source);
    meta.name = symbol;
    meta.path = str::EMPTY_STRING_ID;
    meta.kind = SymbolKind::Foreign;
    meta.generics = TypeIdxRange{};
    const u32 at = static_cast<u32>(state_.external_functions.size());
    state_.external_functions.push_back(
        ExternalFunction{meta, CallingConvention::C});
    externals_by_name.emplace(symbol.offset, at);
    return ExternalFunctionIdx(at);
  };

  const std::vector<bool>& external_mark = reached.of(Reached::Kind::External);
  std::vector<u32> external_map(src.external_functions.size(), U32_MAX);
  for (usize i = 0; i < src.external_functions.size(); ++i) {
    if (!external_mark[i]) {
      continue;
    }
    const ExternalFunction& entry = src.external_functions[i];
    const FunctionMeta mapped_meta = meta_of(entry.meta);
    const auto found = externals_by_name.find(mapped_meta.name.offset);
    if (found != externals_by_name.end()) {
      external_map[i] = found->second;
      continue;
    }
    const u32 at = static_cast<u32>(state_.external_functions.size());
    state_.external_functions.push_back(
        ExternalFunction{mapped_meta, entry.calling_conv});
    externals_by_name.emplace(mapped_meta.name.offset, at);
    external_map[i] = at;
  }

  const std::vector<bool>& type_mark = reached.of(Reached::Kind::Type);
  for (usize i = 0; i < src.types.size(); ++i) {
    if (!type_mark[i]) {
      continue;
    }
    TypeNode made = src.types[TypeIdx(i)];
    using T = TypeTag;
    switch (made.tag) {
      case T::Struct:
        made.data.set(StructTypeIdx(
            mapped(made.as_struct(), struct_map, bases.struct_types).idx));
        break;
      case T::Array:
        made.data.set(ArrayTypeIdx(
            mapped(made.as_array(), array_map, bases.array_types).idx));
        break;
      case T::Slice:
        made.data.set(SliceTypeIdx(
            mapped(made.as_slice(), slice_map, bases.slice_types).idx));
        break;
      case T::Enum:
        made.data.set(EnumTypeIdx(
            mapped(made.as_enum(), enum_map, bases.enum_types).idx));
        break;
      case T::Ref:
      case T::MutRef:
      case T::RawPtr:
      case T::RawMutPtr:
        made.data.set(
            RefTypeIdx(mapped(made.as_ref(), ref_map, bases.ref_types).idx));
        break;
      case T::Tuple:
        made.data.set(TupleTypeIdx(
            mapped(made.as_tuple(), tuple_map, bases.tuple_types).idx));
        break;
      case T::Func:
        made.data.set(FuncTypeIdx(
            mapped(made.as_func(), func_map, bases.func_types).idx));
        break;
      case T::Const:
        made.data.set(ConstTypeIdx(
            mapped(made.as_const(), const_map, bases.const_types).idx));
        break;
      default: break;
    }
    state_.types.emplace_back(made);
  }
  const std::vector<bool>& struct_mark = reached.of(Reached::Kind::Struct);
  for (usize i = 0; i < src.struct_types.size(); ++i) {
    if (!struct_mark[i]) {
      continue;
    }
    const StructType& entry = src.struct_types[StructTypeIdx(i)];
    state_.struct_types.push_back(StructType{
        name(entry.name), type_range(entry.fields), type_range(entry.params)});
  }
  const std::vector<bool>& array_mark = reached.of(Reached::Kind::Array);
  for (usize i = 0; i < src.array_types.size(); ++i) {
    if (!array_mark[i]) {
      continue;
    }
    const ArrayType& entry = src.array_types[ArrayTypeIdx(i)];
    state_.array_types.push_back(ArrayType{type(entry.element), entry.count});
  }
  const std::vector<bool>& slice_mark = reached.of(Reached::Kind::Slice);
  for (usize i = 0; i < src.slice_types.size(); ++i) {
    if (!slice_mark[i]) {
      continue;
    }
    state_.slice_types.push_back(
        SliceType{type(src.slice_types[SliceTypeIdx(i)].element)});
  }
  const std::vector<bool>& enum_mark = reached.of(Reached::Kind::Enum);
  for (usize i = 0; i < src.enum_types.size(); ++i) {
    if (!enum_mark[i]) {
      continue;
    }
    const EnumType& entry = src.enum_types[EnumTypeIdx(i)];
    state_.enum_types.push_back(EnumType{
        name(entry.name),
        mapped_range(entry.variants, variant_map, bases.enum_variant_types),
        type_range(entry.params)});
  }
  const std::vector<bool>& variant_mark =
      reached.of(Reached::Kind::EnumVariant);
  for (usize i = 0; i < src.enum_variant_types.size(); ++i) {
    if (!variant_mark[i]) {
      continue;
    }
    const EnumVariantType& entry =
        src.enum_variant_types[EnumVariantTypeIdx(i)];
    state_.enum_variant_types.push_back(
        EnumVariantType{name(entry.name), type_range(entry.fields)});
  }
  const std::vector<bool>& ref_mark = reached.of(Reached::Kind::Ref);
  for (usize i = 0; i < src.ref_types.size(); ++i) {
    if (!ref_mark[i]) {
      continue;
    }
    state_.ref_types.push_back(
        RefType{type(src.ref_types[RefTypeIdx(i)].pointee)});
  }
  const std::vector<bool>& tuple_mark = reached.of(Reached::Kind::Tuple);
  for (usize i = 0; i < src.tuple_types.size(); ++i) {
    if (!tuple_mark[i]) {
      continue;
    }
    state_.tuple_types.push_back(
        TupleType{type_range(src.tuple_types[TupleTypeIdx(i)].elements)});
  }
  const std::vector<bool>& func_shape_mark = reached.of(Reached::Kind::Func);
  for (usize i = 0; i < src.func_types.size(); ++i) {
    if (!func_shape_mark[i]) {
      continue;
    }
    const FuncType& entry = src.func_types[FuncTypeIdx(i)];
    state_.func_types.push_back(
        FuncType{type_range(entry.params), type(entry.ret)});
  }
  const std::vector<bool>& const_mark = reached.of(Reached::Kind::Const);
  for (usize i = 0; i < src.const_types.size(); ++i) {
    if (!const_mark[i]) {
      continue;
    }
    state_.const_types.push_back(
        ConstType{src.const_types[ConstTypeIdx(i)].value});
  }
  const std::vector<bool>& register_mark = reached.of(Reached::Kind::Register);
  for (usize i = 0; i < src.registers.size(); ++i) {
    if (!register_mark[i]) {
      continue;
    }
    const Register& entry = src.registers[RegisterIdx(i)];
    state_.registers.push_back(
        Register{type(entry.type),
                 mapped(entry.def_idx, instr_map, bases.instructions)});
  }
  const std::vector<bool>& immutable_mark =
      reached.of(Reached::Kind::Immutable);
  for (usize i = 0; i < src.immutables.size(); ++i) {
    if (!immutable_mark[i]) {
      continue;
    }
    const Immutable& entry = src.immutables[ImmutableIdx(i)];
    Immutable made = entry;
    made.type = type(entry.type);
    // A string constant's bytes must exist in the interner the emitters
    // read, so the id moves with the name.
    if (entry.type.is_valid() && entry.type.idx < src.types.size() &&
        src.types[entry.type].tag == TypeTag::Str) {
      made.data.str_id_value = name(entry.data.str_id_value);
    }
    state_.immutables.emplace_back(made);
  }
  const std::vector<bool>& operand_mark = reached.of(Reached::Kind::Operand);
  for (usize i = 0; i < src.operands.size(); ++i) {
    if (!operand_mark[i]) {
      continue;
    }
    Operand made = src.operands[OperandIdx(i)];
    made.type = type(made.type);
    switch (made.tag()) {
      case Operand::TAG_OF<RegisterIdx>:
        made.data = Operand::Payload{
            mapped(made.as_register(), register_map, bases.registers)};
        break;
      case Operand::TAG_OF<FunctionIdx>: {
        const FunctionIdx source = made.as_function();
        const std::optional<str::StringPoolId> symbol = surface_of(source);
        if (symbol.has_value() && source.idx < src.functions.size()) {
          made.data = Operand::Payload{
              external_for(*symbol, src.functions[source.idx].meta)};
          break;
        }
        DCHECK(source.idx < function_map.size());
        made.data = Operand::Payload{FunctionIdx(function_map[source.idx])};
        break;
      }
      case Operand::TAG_OF<BlockIdx>:
        made.data =
            Operand::Payload{mapped(made.as_block(), block_map, bases.blocks)};
        break;
      case Operand::TAG_OF<ImmutableIdx>:
        made.data = Operand::Payload{
            mapped(made.as_immutable(), immutable_map, bases.immutables)};
        break;
      case Operand::TAG_OF<ExternalFunctionIdx>:
        DCHECK(made.as_external_function().idx < external_map.size());
        made.data = Operand::Payload{
            ExternalFunctionIdx(external_map[made.as_external_function().idx])};
        break;
      case Operand::TAG_OF<ParametricCalleeIdx>:
        made.data = Operand::Payload{mapped(
            made.as_parametric_callee(), callee_map, bases.parametric_callees)};
        break;
      default: break;
    }
    state_.operands.emplace_back(made);
  }
  const std::vector<bool>& instr_mark = reached.of(Reached::Kind::Instr);
  for (usize i = 0; i < src.instrs.size(); ++i) {
    if (!instr_mark[i]) {
      continue;
    }
    const Instruction& entry = src.instrs[InstructionIdx(i)];
    state_.instrs.push_back(Instruction{
        .op = entry.op,
        .flags = entry.flags,
        .dst = mapped(entry.dst, register_map, bases.registers),
        .measure = type(entry.measure),
        .operands = mapped_range(entry.operands, operand_map, bases.operands)});
  }
  const std::vector<bool>& block_mark = reached.of(Reached::Kind::Block);
  for (usize i = 0; i < src.blocks.size(); ++i) {
    if (!block_mark[i]) {
      continue;
    }
    const Block& entry = src.blocks[BlockIdx(i)];
    state_.blocks.push_back(Block{
        .instrs = mapped_range(entry.instrs, instr_map, bases.instructions),
        .block_params = mapped_range(entry.block_params, block_param_map,
                                     bases.block_params)});
  }
  const std::vector<bool>& block_param_mark =
      reached.of(Reached::Kind::BlockParam);
  for (usize i = 0; i < src.block_params.size(); ++i) {
    if (!block_param_mark[i]) {
      continue;
    }
    const BlockParam& entry = src.block_params[BlockParamIdx(i)];
    state_.block_params.push_back(BlockParam{
        type(entry.type), mapped(entry.reg, register_map, bases.registers)});
  }
  const std::vector<bool>& function_mark = reached.of(Reached::Kind::Function);
  for (usize i = 0; i < src.functions.size(); ++i) {
    if (!function_mark[i]) {
      continue;
    }
    const Function& entry = src.functions[FunctionIdx(i)];
    DCHECK_EQ(function_map[i], state_.functions.size());
    state_.functions.push_back(Function{
        .meta = meta_of(entry.meta),
        .blocks = mapped_range(entry.blocks, block_map, bases.blocks)});
  }
  const std::vector<bool>& callee_mark =
      reached.of(Reached::Kind::ParametricCallee);
  for (usize i = 0; i < src.parametric_callees.size(); ++i) {
    if (!callee_mark[i]) {
      continue;
    }
    const ParametricCallee& entry =
        src.parametric_callees[ParametricCalleeIdx(i)];
    state_.parametric_callees.push_back(ParametricCallee{
        .kind = entry.kind,
        .function = entry.function.is_valid()
                        ? FunctionIdx(function_map[entry.function.idx])
                        : entry.function,
        .spec = name(entry.spec),
        .method = name(entry.method),
        .args = type_range(entry.args)});
  }
  for (const FunctionIdx function : src.parametric_functions) {
    if (function.is_valid() && function.idx < function_map.size() &&
        function_map[function.idx] != U32_MAX) {
      state_.parametric_functions.emplace_back(function_map[function.idx]);
    }
  }
  for (const FunctionIdx function : src.prelude_specializations) {
    if (function.is_valid() && function.idx < function_map.size() &&
        function_map[function.idx] != U32_MAX) {
      state_.prelude_specializations.emplace_back(function_map[function.idx]);
    }
  }
  for (const FunctionIdx function : src.synthetic_functions) {
    if (function.is_valid() && function.idx < function_map.size() &&
        function_map[function.idx] != U32_MAX) {
      state_.synthetic_functions.emplace_back(function_map[function.idx]);
    }
  }
  // The interning tables describe the table that just grew.
  index_interned();
  return bases;
}

}  // namespace ir
