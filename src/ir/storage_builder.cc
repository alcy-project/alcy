// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/storage_builder.h"

#include <functional>
#include <optional>
#include <unordered_map>
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
Idx shifted(Idx idx, u32 base) {
  return idx.is_valid() ? Idx(idx.idx + base) : idx;
}

// A range into an appended table: the entries stay consecutive, so a
// whole range moves by its table's base. An empty range is not a
// reference and keeps its head.
template <typename T>
base::IdxRange<T> shifted_range(base::IdxRange<T> range, u32 base) {
  if (range.size() == 0) {
    return range;
  }
  return base::IdxRange<T>(T(range.head().idx + base), range.size());
}

}  // namespace

StorageBases StorageBuilder::append_storage(
    const Storage& other,
    const std::function<str::StringPoolId(str::StringPoolId)>& map_string,
    const SurfaceResolver& surface_of) {
  const StorageState& src = other.state();
  // Every base is the table's size before the append: an entry read from
  // the appended half names its base plus the index it had there.
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

  const auto type = [&](TypeIdx idx) { return shifted(idx, bases.types); };
  const auto type_range = [&](TypeIdxRange range) {
    return shifted_range(range, bases.types);
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

  std::vector<u32> external_map(src.external_functions.size(), 0);
  // Where each carried function lands, decided before any reference to
  // one is rewritten: a function a symbol answers for stays absent.
  bases.function_map.assign(src.functions.size(), U32_MAX);
  {
    u32 next_function = static_cast<u32>(state_.functions.size());
    for (usize i = 0; i < src.functions.size(); ++i) {
      if (!surface_of(FunctionIdx(static_cast<u32>(i))).has_value()) {
        bases.function_map[i] = next_function++;
      }
    }
  }
  for (usize i = 0; i < src.external_functions.size(); ++i) {
    const ExternalFunction& entry = src.external_functions[i];
    const FunctionMeta mapped = meta_of(entry.meta);
    const auto found = externals_by_name.find(mapped.name.offset);
    if (found != externals_by_name.end()) {
      external_map[i] = found->second;
      continue;
    }
    const u32 at = static_cast<u32>(state_.external_functions.size());
    state_.external_functions.push_back(
        ExternalFunction{mapped, entry.calling_conv});
    externals_by_name.emplace(mapped.name.offset, at);
    external_map[i] = at;
  }

  for (const TypeNode& node : src.types) {
    TypeNode made = node;
    using T = TypeTag;
    switch (node.tag) {
      case T::Struct:
        made.data.set(StructTypeIdx(node.as_struct().idx + bases.struct_types));
        break;
      case T::Array:
        made.data.set(ArrayTypeIdx(node.as_array().idx + bases.array_types));
        break;
      case T::Slice:
        made.data.set(SliceTypeIdx(node.as_slice().idx + bases.slice_types));
        break;
      case T::Enum:
        made.data.set(EnumTypeIdx(node.as_enum().idx + bases.enum_types));
        break;
      case T::Ref:
      case T::MutRef:
      case T::RawPtr:
      case T::RawMutPtr:
        made.data.set(RefTypeIdx(node.as_ref().idx + bases.ref_types));
        break;
      case T::Tuple:
        made.data.set(TupleTypeIdx(node.as_tuple().idx + bases.tuple_types));
        break;
      case T::Func:
        made.data.set(FuncTypeIdx(node.as_func().idx + bases.func_types));
        break;
      case T::Const:
        made.data.set(ConstTypeIdx(node.as_const().idx + bases.const_types));
        break;
      default: break;
    }
    state_.types.emplace_back(made);
  }
  for (const StructType& entry : src.struct_types) {
    state_.struct_types.push_back(StructType{
        name(entry.name), type_range(entry.fields), type_range(entry.params)});
  }
  for (const ArrayType& entry : src.array_types) {
    state_.array_types.push_back(ArrayType{type(entry.element), entry.count});
  }
  for (const SliceType& entry : src.slice_types) {
    state_.slice_types.push_back(SliceType{type(entry.element)});
  }
  for (const EnumType& entry : src.enum_types) {
    state_.enum_types.push_back(
        EnumType{name(entry.name),
                 shifted_range(entry.variants, bases.enum_variant_types),
                 type_range(entry.params)});
  }
  for (const EnumVariantType& entry : src.enum_variant_types) {
    state_.enum_variant_types.push_back(
        EnumVariantType{name(entry.name), type_range(entry.fields)});
  }
  for (const RefType& entry : src.ref_types) {
    state_.ref_types.push_back(RefType{type(entry.pointee)});
  }
  for (const TupleType& entry : src.tuple_types) {
    state_.tuple_types.push_back(TupleType{type_range(entry.elements)});
  }
  for (const FuncType& entry : src.func_types) {
    state_.func_types.push_back(
        FuncType{type_range(entry.params), type(entry.ret)});
  }
  for (const ConstType& entry : src.const_types) {
    state_.const_types.push_back(ConstType{entry.value});
  }
  for (const Register& entry : src.registers) {
    state_.registers.push_back(
        Register{type(entry.type), shifted(entry.def_idx, bases.instructions)});
  }
  for (const Immutable& entry : src.immutables) {
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
  for (const Operand& entry : src.operands) {
    Operand made = entry;
    made.type = type(entry.type);
    switch (entry.tag()) {
      case Operand::TAG_OF<RegisterIdx>:
        made.data =
            Operand::Payload{shifted(entry.as_register(), bases.registers)};
        break;
      case Operand::TAG_OF<FunctionIdx>: {
        const FunctionIdx source = entry.as_function();
        const std::optional<str::StringPoolId> symbol = surface_of(source);
        if (symbol.has_value() && source.idx < src.functions.size()) {
          made.data = Operand::Payload{
              external_for(*symbol, src.functions[source.idx].meta)};
          break;
        }
        made.data =
            Operand::Payload{FunctionIdx(source.idx < bases.function_map.size()
                                             ? bases.function_map[source.idx]
                                             : U32_MAX)};
        break;
      }
      case Operand::TAG_OF<BlockIdx>:
        made.data = Operand::Payload{shifted(entry.as_block(), bases.blocks)};
        break;
      case Operand::TAG_OF<ImmutableIdx>:
        made.data =
            Operand::Payload{shifted(entry.as_immutable(), bases.immutables)};
        break;
      case Operand::TAG_OF<ExternalFunctionIdx>:
        made.data = Operand::Payload{ExternalFunctionIdx(
            external_map[entry.as_external_function().idx])};
        break;
      case Operand::TAG_OF<ParametricCalleeIdx>:
        made.data = Operand::Payload{
            shifted(entry.as_parametric_callee(), bases.parametric_callees)};
        break;
      default: break;
    }
    state_.operands.emplace_back(made);
  }
  for (const Instruction& entry : src.instrs) {
    state_.instrs.push_back(
        Instruction{.op = entry.op,
                    .flags = entry.flags,
                    .dst = shifted(entry.dst, bases.registers),
                    .measure = type(entry.measure),
                    .operands = shifted_range(entry.operands, bases.operands)});
  }
  for (const Block& entry : src.blocks) {
    state_.blocks.push_back(Block{
        .instrs = shifted_range(entry.instrs, bases.instructions),
        .block_params = shifted_range(entry.block_params, bases.block_params)});
  }
  for (const BlockParam& entry : src.block_params) {
    state_.block_params.push_back(
        BlockParam{type(entry.type), shifted(entry.reg, bases.registers)});
  }
  for (usize i = 0; i < src.functions.size(); ++i) {
    const u32 mapped = bases.function_map[i];
    if (mapped == U32_MAX) {
      // The code is the package's own object's; the row is not carried,
      // and every reference to it names the symbol instead.
      continue;
    }
    const Function& entry = src.functions[i];
    DCHECK_EQ(mapped, state_.functions.size());
    state_.functions.push_back(
        Function{.meta = meta_of(entry.meta),
                 .blocks = shifted_range(entry.blocks, bases.blocks)});
  }
  for (const ParametricCallee& entry : src.parametric_callees) {
    state_.parametric_callees.push_back(
        ParametricCallee{.kind = entry.kind,
                         .function = shifted(entry.function, bases.functions),
                         .spec = name(entry.spec),
                         .method = name(entry.method),
                         .args = type_range(entry.args)});
  }
  for (const FunctionIdx function : src.parametric_functions) {
    if (function.idx < bases.function_map.size()) {
      state_.parametric_functions.emplace_back(
          bases.function_map[function.idx]);
    }
  }
  for (const FunctionIdx function : src.prelude_specializations) {
    if (function.idx < bases.function_map.size()) {
      state_.prelude_specializations.emplace_back(
          bases.function_map[function.idx]);
    }
  }
  for (const FunctionIdx function : src.synthetic_functions) {
    if (function.idx < bases.function_map.size()) {
      state_.synthetic_functions.emplace_back(bases.function_map[function.idx]);
    }
  }
  // The interning tables describe the table that just grew.
  index_interned();
  return bases;
}

}  // namespace ir
