// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Specialization (ADR-0079): every parametric callee a lowered body
// carries is replaced by the monomorphic function its arguments decide.
// The definition-site body is cloned once per (item, arguments) pair, its
// `Param` nodes substituted, and the clone's own records resolved in
// turn. The pass runs after every body lowered and before the storage is
// built, so the borrow pass and the backends meet monomorphic code only.

#include <bit>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/fmt.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/parametric.h"
#include "ir/register.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "lowering/drop_elaboration.h"
#include "lowering/immediate.h"
#include "lowering/lowerer.h"
#include "lowering/lowering.h"
#include "source/source.h"

namespace lowering {

namespace {

// The builtin answer to a spec method on a scalar: the opcode the
// concrete body would have lowered to, and whether the expansion
// writes a zero of the receiver's type as its first operand (`-x` is
// `0 - x`, the spelling the lowering uses).
struct BuiltinOperator {
  ir::Opcode op;
  bool zero_first_operand = false;
};

// A capability a scalar satisfies without an implementation. A nominal
// type reaches its own implementation instead, and the specialization
// resolves that call (ADR-0079).
std::optional<BuiltinOperator> builtin_operator(std::string_view spec,
                                                std::string_view method,
                                                ir::TypeTag tag) {
  using O = ir::Opcode;
  // A boolean is no arithmetic scalar: the analyzer refuses `+` on one,
  // so the builtin must not answer for it either.
  const bool integer = ir::is_integer_type(tag) && tag != ir::TypeTag::I1;
  const bool floating = ir::is_float_type(tag);
  const bool numeric = integer || floating;
  const bool comparable = numeric || ir::is_pointer_type(tag);
  if (spec == "PartialEq" || spec == "Eq") {
    if (!comparable) {
      return std::nullopt;
    }
    // `==` and `!=` both reach `eq`; the caller negates the result.
    return method == "eq"
               ? std::optional<BuiltinOperator>(BuiltinOperator{O::Eq})
               : std::nullopt;
  }
  if (!numeric) {
    return std::nullopt;
  }
  if (spec == "Add") {
    return BuiltinOperator{floating ? O::FAdd : O::IntAdd};
  }
  if (spec == "Sub") {
    return BuiltinOperator{floating ? O::FSub : O::IntSub};
  }
  if (spec == "Mul") {
    return BuiltinOperator{floating ? O::FMul : O::IntMul};
  }
  if (spec == "Div") {
    if (floating) {
      return BuiltinOperator{O::FDiv};
    }
    return BuiltinOperator{ir::is_signed_integer_type(tag) ? O::IntDiv
                                                           : O::UintDiv};
  }
  if (spec == "Rem" && integer) {
    return BuiltinOperator{ir::is_signed_integer_type(tag) ? O::IntRem
                                                           : O::UintRem};
  }
  if (spec == "Neg") {
    return BuiltinOperator{floating ? O::FSub : O::IntSub,
                           /*zero_first_operand=*/true};
  }
  return std::nullopt;
}

// Whether the opcode compares its operands, which the spec's method
// borrows: the builtin compares the values, so each operand is read
// through its reference first.
bool comparison_opcode(ir::Opcode op) {
  return op == ir::Opcode::Eq || op == ir::Opcode::Ne || op == ir::Opcode::Lt ||
         op == ir::Opcode::Le || op == ir::Opcode::Gt || op == ir::Opcode::Ge;
}

}  // namespace

void Lowerer::specialization_key(ir::FunctionIdx source,
                                 const std::vector<ir::TypeIdx>& args,
                                 std::string& key) {
  key.clear();
  append_index(key, source.idx, '|');
  for (const ir::TypeIdx arg : args) {
    // Two call sites may hand the same type as different storage
    // copies; the identity that memoizes the clone is the type, not
    // the copy.
    append_index(key, canonical_type(arg).idx, ',');
  }
}

ir::TypeIdx Lowerer::canonical_type(ir::TypeIdx type) const {
  return canonical_origin(type_origins_, type);
}

// Structural equality after the origin chains: two storage copies of
// one type, and two instances over the same declaration and arguments,
// are one type wherever a table is keyed by identity.
bool Lowerer::type_equivalent(ir::TypeIdx a, ir::TypeIdx b) const {
  a = canonical_type(a);
  b = canonical_type(b);
  if (a.idx == b.idx) {
    return true;
  }
  if (a.idx >= builder.state().types.size() ||
      b.idx >= builder.state().types.size()) {
    return false;
  }
  const ir::TypeNode lhs = builder.state().types[a];
  const ir::TypeNode rhs = builder.state().types[b];
  if (lhs.tag != rhs.tag) {
    return false;
  }
  using T = ir::TypeTag;
  switch (lhs.tag) {
    case T::Ref:
    case T::MutRef:
    case T::RawPtr:
    case T::RawMutPtr:
      return type_equivalent(builder.state().ref_types[lhs.as_ref()].pointee,
                             builder.state().ref_types[rhs.as_ref()].pointee);
    case T::Array:
      return builder.state().array_types[lhs.as_array()].count ==
                 builder.state().array_types[rhs.as_array()].count &&
             type_equivalent(
                 builder.state().array_types[lhs.as_array()].element,
                 builder.state().array_types[rhs.as_array()].element);
    case T::Slice:
      return type_equivalent(
          builder.state().slice_types[lhs.as_slice()].element,
          builder.state().slice_types[rhs.as_slice()].element);
    case T::Tuple: {
      const ir::TypeIdxRange left =
          builder.state().tuple_types[lhs.as_tuple()].elements;
      const ir::TypeIdxRange right =
          builder.state().tuple_types[rhs.as_tuple()].elements;
      if (left.size() != right.size()) {
        return false;
      }
      for (u32 i = 0; i < left.size(); ++i) {
        if (!type_equivalent(left[i], right[i])) {
          return false;
        }
      }
      return true;
    }
    case T::Struct: {
      // Two indexes of one declaration are the same type: a non-generic
      // nominal is interned, but a copy or a re-mint is not.
      const analyzer::CheckedModule::StructInfo* left = struct_info(a);
      const analyzer::CheckedModule::StructInfo* right = struct_info(b);
      if (left != nullptr && right != nullptr && left->item == right->item) {
        const ir::StructType left_shape =
            builder.state().struct_types[lhs.as_struct()];
        const ir::StructType right_shape =
            builder.state().struct_types[rhs.as_struct()];
        if (left_shape.params.size() != right_shape.params.size()) {
          return false;
        }
        for (u32 i = 0; i < left_shape.params.size(); ++i) {
          if (!type_equivalent(left_shape.params[i], right_shape.params[i])) {
            return false;
          }
        }
        return true;
      }
      return false;
    }
    case T::Enum: {
      const auto left = enums_.find(a.idx);
      const auto right = enums_.find(b.idx);
      if (left != enums_.end() && right != enums_.end() &&
          left->second->item == right->second->item) {
        const ir::EnumType left_shape =
            builder.state().enum_types[lhs.as_enum()];
        const ir::EnumType right_shape =
            builder.state().enum_types[rhs.as_enum()];
        if (left_shape.params.size() != right_shape.params.size()) {
          return false;
        }
        for (u32 i = 0; i < left_shape.params.size(); ++i) {
          if (!type_equivalent(left_shape.params[i], right_shape.params[i])) {
            return false;
          }
        }
        return true;
      }
      return false;
    }
    case T::Func: {
      const ir::FuncType left = builder.state().func_types[lhs.as_func()];
      const ir::FuncType right = builder.state().func_types[rhs.as_func()];
      if (left.params.size() != right.params.size() ||
          !type_equivalent(left.ret, right.ret)) {
        return false;
      }
      for (u32 i = 0; i < left.params.size(); ++i) {
        if (!type_equivalent(left.params[i], right.params[i])) {
          return false;
        }
      }
      return true;
    }
    default: return false;
  }
}

// The instance of a nominal declaration at `args`, found among the ones
// already interned for its item. A partial instance names a parameter
// and is skipped: the substitution must mint a whole shape instead.
ir::TypeIdx Lowerer::nominal_instance(ast::ItemIdx item,
                                      const std::vector<ir::TypeIdx>& args) {
  const auto matches = [&](const std::vector<ir::TypeIdx>& candidates,
                           bool is_enum) -> ir::TypeIdx {
    for (const ir::TypeIdx candidate : candidates) {
      const ir::TypeNode node =
          builder.state().types[canonical_type(candidate)];
      if (is_enum ? node.tag != ir::TypeTag::Enum
                  : node.tag != ir::TypeTag::Struct) {
        continue;
      }
      // A partially substituted instance has the right arguments and
      // the wrong shape; the substitution must mint a whole one.
      if (ir::type_names_param(builder.state(), candidate)) {
        continue;
      }
      ir::TypeIdxRange params{};
      if (is_enum) {
        params = builder.state().enum_types[node.as_enum()].params;
      } else {
        params = builder.state().struct_types[node.as_struct()].params;
      }
      if (params.size() != args.size()) {
        continue;
      }
      bool same = true;
      for (u32 i = 0; same && i < args.size(); ++i) {
        same = type_equivalent(params[i], args[i]);
      }
      if (same) {
        return candidate;
      }
    }
    return ir::TypeIdx::invalid();
  };
  const auto strukt = struct_instances_.find(item.idx);
  if (strukt != struct_instances_.end()) {
    const ir::TypeIdx instance = matches(strukt->second, false);
    if (instance.is_valid()) {
      return instance;
    }
  }
  const auto enumeration = enum_instances_.find(item.idx);
  if (enumeration != enum_instances_.end()) {
    const ir::TypeIdx instance = matches(enumeration->second, true);
    if (instance.is_valid()) {
      return instance;
    }
  }
  return ir::TypeIdx::invalid();
}

ir::TypeIdx Lowerer::mint_nominal(ir::TypeIdx generic_type,
                                  ast::ItemIdx item,
                                  const std::vector<ir::TypeIdx>& args) {
  const ir::TypeNode node = builder.state().types[generic_type];
  ir::TypeSeq params;
  for (const ir::TypeIdx arg : args) {
    params.push(storage_copy(arg));
  }
  const auto remember = [&](ir::TypeIdx minted) {
    if (item.is_valid()) {
      struct_instances_[item.idx].push_back(minted);
    }
  };
  // A nominal minted from a summary shape takes what the declaration
  // measured: the tables grow parallel to the type table, and the
  // destructor marker stays visible so ending the instance refuses a
  // body that did not cross (ADR-0085).
  const auto inherit_summary = [&](ir::TypeIdx minted) {
    ensure_traits_parallel();
    const u8 needs = generic_type.idx < pkg.needs_drop.size()
                         ? pkg.needs_drop[generic_type.idx]
                         : u8{0};
    const u8 anchor = generic_type.idx < pkg.not_movable.size()
                          ? pkg.not_movable[generic_type.idx]
                          : u8{0};
    if (needs != 0) {
      pkg.needs_drop[minted.idx] = 1;
      summary_drops_[minted.idx] = 1;
    }
    if (anchor != 0) {
      pkg.not_movable[minted.idx] = 1;
    }
  };
  if (node.tag == ir::TypeTag::Struct) {
    const ir::StructType shape = builder.state().struct_types[node.as_struct()];
    // The row exists before its fields do: a field may name the very
    // instance being minted (a recursive type), and the lookup must
    // find something.
    const ir::TypeIdx minted =
        builder.struct_type(shape.name, ir::TypeIdxRange{}, params.finish());
    remember(minted);
    inherit_summary(minted);
    named_struct_instances_[shape.name.offset].push_back(minted);
    std::vector<ir::TypeIdx> fields;
    fields.reserve(shape.fields.size());
    for (const ir::TypeIdx field : shape.fields) {
      fields.push_back(substitute_type(field, args));
    }
    ir::TypeSeq field_seq;
    for (const ir::TypeIdx field : fields) {
      field_seq.push(storage_copy(field));
    }
    builder.fill_struct(minted, field_seq.finish(), params.finish());
    return minted;
  }
  if (node.tag != ir::TypeTag::Enum) {
    internal(diag::Span{}, "a specialization mints a non-nominal type");
    return generic_type;
  }
  const ir::EnumType shape = builder.state().enum_types[node.as_enum()];
  const ir::TypeIdx minted = builder.enum_type(
      shape.name, ir::EnumVariantTypeIdxRange{}, params.finish());
  remember(minted);
  inherit_summary(minted);
  named_enum_instances_[shape.name.offset].push_back(minted);
  ir::EnumVariantTypeSeq variants;
  for (const ir::EnumVariantTypeIdx vidx : shape.variants) {
    const ir::EnumVariantType variant =
        builder.state().enum_variant_types[vidx];
    std::vector<ir::TypeIdx> fields;
    fields.reserve(variant.fields.size());
    for (const ir::TypeIdx field : variant.fields) {
      fields.push_back(substitute_type(field, args));
    }
    ir::TypeSeq field_seq;
    for (const ir::TypeIdx field : fields) {
      field_seq.push(storage_copy(field));
    }
    variants.push(builder.enum_variant(variant.name, field_seq.finish()));
  }
  builder.fill_enum(minted, variants.finish(), params.finish());
  return minted;
}

ir::TypeIdx Lowerer::substitute_type(ir::TypeIdx type,
                                     const std::vector<ir::TypeIdx>& args) {
  if (!type.is_valid() || type.idx >= builder.state().types.size()) {
    return type;
  }
  // A storage copy answers from the type it came from, or its instance
  // shape cannot be found.
  type = canonical_type(type);
  const ir::TypeNode node = builder.state().types[type];
  using T = ir::TypeTag;
  switch (node.tag) {
    case T::Param: {
      const u32 index = node.as_param().value;
      if (index >= args.size()) {
        // The record and the clone disagree about the parameter list;
        // leaving the node would put a Param in a resolved body, which
        // no layout answers.
        internal(diag::Span{}, "a substitution misses a type argument");
        return type;
      }
      return args[index];
    }
    case T::Ref:
    case T::MutRef: {
      const ir::TypeIdx pointee =
          builder.state().ref_types[node.as_ref()].pointee;
      const ir::TypeIdx substituted = substitute_type(pointee, args);
      if (substituted.idx == pointee.idx) {
        return type;
      }
      return builder.reference_type(substituted, node.tag == T::MutRef);
    }
    case T::RawPtr:
    case T::RawMutPtr: {
      const ir::TypeIdx pointee =
          builder.state().ref_types[node.as_ref()].pointee;
      const ir::TypeIdx substituted = substitute_type(pointee, args);
      if (substituted.idx == pointee.idx) {
        return type;
      }
      return builder.raw_pointer_type(substituted, node.tag == T::RawMutPtr);
    }
    case T::Array: {
      const ir::ArrayType shape = builder.state().array_types[node.as_array()];
      const ir::TypeIdx element = substitute_type(shape.element, args);
      if (element.idx == shape.element.idx) {
        return type;
      }
      return builder.array_type(element, shape.count);
    }
    case T::Slice: {
      const ir::TypeIdx element =
          builder.state().slice_types[node.as_slice()].element;
      const ir::TypeIdx substituted = substitute_type(element, args);
      return substituted.idx == element.idx ? type
                                            : builder.slice_type(substituted);
    }
    case T::Tuple: {
      const ir::TypeIdxRange elements =
          builder.state().tuple_types[node.as_tuple()].elements;
      // Substitution may intern new types, so it runs before the range
      // is built: the pushed entries must stay consecutive.
      std::vector<ir::TypeIdx> substituted;
      substituted.reserve(elements.size());
      bool changed = false;
      for (const ir::TypeIdx element : elements) {
        const ir::TypeIdx replaced = substitute_type(element, args);
        changed = changed || replaced.idx != element.idx;
        substituted.push_back(replaced);
      }
      if (!changed) {
        return type;
      }
      ir::TypeSeq seq;
      for (const ir::TypeIdx element : substituted) {
        seq.push(storage_copy(element));
      }
      return builder.tuple_type(seq.finish());
    }
    case T::Func: {
      const ir::FuncType shape = builder.state().func_types[node.as_func()];
      std::vector<ir::TypeIdx> params;
      bool changed = false;
      for (const ir::TypeIdx param : shape.params) {
        const ir::TypeIdx substituted = substitute_type(param, args);
        changed = changed || substituted.idx != param.idx;
        params.push_back(substituted);
      }
      const ir::TypeIdx ret = substitute_type(shape.ret, args);
      changed = changed || ret.idx != shape.ret.idx;
      if (!changed) {
        return type;
      }
      ir::TypeSeq seq;
      for (const ir::TypeIdx param : params) {
        seq.push(storage_copy(param));
      }
      return builder.func_type(seq.finish(), ret);
    }
    case T::Struct: {
      if (!ir::type_names_param(builder.state(), type)) {
        return type;
      }
      const analyzer::CheckedModule::StructInfo* info = struct_info(type);
      const ir::StructType shape =
          builder.state().struct_types[node.as_struct()];
      std::vector<ir::TypeIdx> instance_args;
      instance_args.reserve(shape.params.size());
      for (const ir::TypeIdx param : shape.params) {
        instance_args.push_back(substitute_type(param, args));
      }
      if (info == nullptr) {
        // A compiler-owned wrapper (`MaybeUninit`) carries its argument
        // in its field, not in `params`; the substitution fills the
        // fields and the name plus the fields identify the instance.
        std::vector<ir::TypeIdx> substituted_fields;
        substituted_fields.reserve(shape.fields.size());
        for (const ir::TypeIdx field : shape.fields) {
          substituted_fields.push_back(substitute_type(field, args));
        }
        for (const ir::TypeIdx candidate :
             named_struct_instances_[shape.name.offset]) {
          if (ir::type_names_param(builder.state(), candidate)) {
            continue;
          }
          const ir::TypeNode candidate_node =
              builder.state().types[canonical_type(candidate)];
          if (candidate_node.tag != ir::TypeTag::Struct) {
            continue;
          }
          const ir::StructType candidate_shape =
              builder.state().struct_types[candidate_node.as_struct()];
          if (candidate_shape.fields.size() != substituted_fields.size()) {
            continue;
          }
          bool same = true;
          for (u32 i = 0; same && i < substituted_fields.size(); ++i) {
            same = type_equivalent(candidate_shape.fields[i],
                                   substituted_fields[i]);
          }
          if (same) {
            return candidate;
          }
        }
        // The fields were substituted before the row exists, so a
        // recursive wrapper cannot be met above; mint it whole.
        ir::TypeSeq field_seq;
        for (const ir::TypeIdx field : substituted_fields) {
          field_seq.push(storage_copy(field));
        }
        const ir::TypeIdx minted = builder.struct_type(
            shape.name, field_seq.finish(), ir::TypeIdxRange{});
        named_struct_instances_[shape.name.offset].push_back(minted);
        return minted;
      }
      const ir::TypeIdx instance = nominal_instance(info->item, instance_args);
      if (instance.is_valid()) {
        return instance;
      }
      return mint_nominal(type, info->item, instance_args);
    }
    case T::Enum: {
      if (!ir::type_names_param(builder.state(), type)) {
        return type;
      }
      const auto found = enums_.find(canonical_type(type).idx);
      if (found == enums_.end()) {
        return type;
      }
      const ir::EnumType shape = builder.state().enum_types[node.as_enum()];
      std::vector<ir::TypeIdx> instance_args;
      instance_args.reserve(shape.params.size());
      for (const ir::TypeIdx param : shape.params) {
        instance_args.push_back(substitute_type(param, args));
      }
      const ir::TypeIdx instance =
          nominal_instance(found->second->item, instance_args);
      if (instance.is_valid()) {
        return instance;
      }
      return mint_nominal(type, found->second->item, instance_args);
    }
    default:
      // A tag with no parameter inside it - a primitive, a const
      // argument - is already the type the clone names; a parameter is
      // caught above, so nothing here can leave one behind.
      return type;
  }
}

ir::FunctionIdx Lowerer::specialize_generic(
    ast::ItemIdx item,
    const std::vector<ir::TypeIdx>& args,
    diag::Span span) {
  const auto parametric = parametric_fn_by_item_.find(item.idx);
  if (parametric == parametric_fn_by_item_.end()) {
    internal(span, "a generic call has no parametric body");
    return ir::FunctionIdx::invalid();
  }
  return specialize_body(ir::FunctionIdx(parametric->second), args, span);
}

ir::FunctionIdx Lowerer::specialize_body(ir::FunctionIdx source_idx,
                                         const std::vector<ir::TypeIdx>& args,
                                         diag::Span instantiation_span) {
  specialization_key(source_idx, args, specialization_key_scratch_);
  const auto found = specialization_by_key_.find(
      std::string_view(specialization_key_scratch_));
  if (found != specialization_by_key_.end()) {
    return ir::FunctionIdx(found->second);
  }
  const ir::Function source = builder.state().functions[source_idx];
  // The substitutions run first: one may intern new types, and the
  // copies the range holds must stay consecutive.
  std::vector<ir::TypeIdx> substituted_params;
  substituted_params.reserve(source.meta.param_types.size());
  for (const ir::TypeIdx param : source.meta.param_types) {
    substituted_params.push_back(substitute_type(param, args));
  }
  ir::TypeSeq param_types;
  for (const ir::TypeIdx param : substituted_params) {
    param_types.push(storage_copy(param));
  }
  const ir::TypeIdx ret = substitute_type(source.meta.return_type, args);
  ir::TypeSeq generics;
  for (const ir::TypeIdx arg : args) {
    generics.push(storage_copy(arg));
  }
  const ir::TypeIdx owner = source.meta.owner.is_valid()
                                ? substitute_type(source.meta.owner, args)
                                : source.meta.owner;
  const ir::FunctionMeta meta{.return_type = ret,
                              .param_types = param_types.finish(),
                              .name = source.meta.name,
                              .path = source.meta.path,
                              .kind = source.meta.kind,
                              .generics = generics.finish(),
                              .owner = owner};
  const ir::FunctionIdx clone = reserve_body_function(meta);
  // Memoize before cloning: a recursive instantiation reaches its own
  // clone by key while the body is still being built.
  specialization_by_key_.emplace(specialization_key_scratch_, clone.idx);
  // A clone of a staged item is no source function either, and a
  // reported count excludes it (ADR-0079). A body carried from a
  // summary has no entry here; its clone counts with the dependent's
  // own functions.
  if (source_idx.idx < fns.size()) {
    const u32 module = fns[source_idx.idx].mod;
    if (module < pkg.tree.modules.size() &&
        pkg.tree.modules[module]->is_staged) {
      builder.add_prelude_specialization(clone);
    }
  }
  clone_body(source_idx, clone, args, instantiation_span);
  return clone;
}

ir::RegisterIdx Lowerer::clone_register(ir::RegisterIdx source,
                                        ir::TypeIdx type,
                                        CloneState& state) {
  if (!source.is_valid()) {
    return ir::RegisterIdx::invalid();
  }
  const auto found = state.registers.find(source.idx);
  if (found != state.registers.end()) {
    return found->second;
  }
  if (!type.is_valid()) {
    type = substitute_type(builder.state().registers[source].type, *state.args);
  }
  const ir::RegisterIdx clone = builder.reg(
      ir::Register{.type = type, .def_idx = ir::InstructionIdx::invalid()});
  state.registers.emplace(source.idx, clone);
  const auto name = state.addr_names.find(source.idx);
  if (name != state.addr_names.end()) {
    const LoweredPackage::AddrInfo info = name->second;
    addr_names_.push_back(LoweredPackage::AddrInfo{
        clone, info.name, info.is_param, info.is_capture});
  }
  return clone;
}

void Lowerer::translate_instruction(ir::InstructionIdx source,
                                    CloneState& state,
                                    std::vector<StagedInstruction>& out) {
  const ir::Instruction instr = builder.state().instrs[source];
  const ir::OperandIdxRange operands = instr.operands;
  const auto translate_operand = [&](const ir::Operand& operand) {
    using Payload = ir::Operand::Payload;
    switch (operand.tag()) {
      case Payload::TagOf<ir::RegisterIdx>:
        return ir::Operand::from_register(
            clone_register(operand.as_register(), ir::TypeIdx::invalid(),
                           state),
            substitute_type(operand.type, *state.args));
      case Payload::TagOf<ir::BlockIdx>: {
        const auto block = state.blocks.find(operand.as_block().idx);
        if (block == state.blocks.end()) {
          internal(diag::Span{}, "a clone names a block it did not reserve");
          failed = true;
          return ir::Operand::invalid();
        }
        return ir::Operand::from_block(block->second, operand.type);
      }
      case Payload::TagOf<ir::ImmutableIdx>: {
        if (!ir::type_names_param(builder.state(), operand.type)) {
          return ir::Operand{operand};
        }
        const ir::Immutable immutable =
            builder.state().immutables[operand.as_immutable()];
        const ir::TypeIdx type = substitute_type(operand.type, *state.args);
        if (tag_of(operand.type) == ir::TypeTag::Param) {
          // A literal whose width the substitution settles: an integer
          // re-encodes to the named width, a float from its IEEE bits to
          // the named kind (ADR-0079).
          const ir::TypeTag target = tag_of(type);
          if (target == ir::TypeTag::F32 || target == ir::TypeTag::F64) {
            const f64 value = std::bit_cast<f64>(immutable.data.u64_value);
            ir::Immutable retyped{.type = type, .data = {}};
            if (target == ir::TypeTag::F32) {
              retyped.data.f32_value = static_cast<f32>(value);
            } else {
              retyped.data.f64_value = value;
            }
            return ir::Operand::from_immutable(builder.immutable(retyped),
                                               type);
          }
          const ir::Immutable retyped =
              immutable_from_u64(target, type, immutable.data.u64_value);
          return ir::Operand::from_immutable(builder.immutable(retyped), type);
        }
        return ir::Operand::from_immutable(
            builder.immutable(
                ir::Immutable{.type = type, .data = immutable.data}),
            type);
      }
      case Payload::TagOf<ir::FunctionIdx>:
      case Payload::TagOf<ir::ExternalFunctionIdx>:
      default: return ir::Operand{operand};
    }
  };

  StagedInstruction staged;
  staged.op = instr.op;
  staged.flags = instr.flags;
  staged.dst = clone_register(instr.dst, ir::TypeIdx::invalid(), state);
  staged.measure = instr.measure.is_valid()
                       ? substitute_type(instr.measure, *state.args)
                       : ir::TypeIdx::invalid();
  // A body carried from a summary has no spans: its instructions name
  // the call that instantiated it, which is where a reader can look
  // (ADR-0079).
  staged.span = source.idx < instr_spans_.size() &&
                        instr_spans_[source.idx].file != source::UNKNOWN_FILE
                    ? instr_spans_[source.idx]
                    : state.instantiation_span;
  if (instr.op == ir::Opcode::ParametricDrop) {
    const ir::Operand place =
        translate_operand(builder.state().operands[operands.head()]);
    if (failed) {
      return;
    }
    resolve_parametric_drop(place, instr.measure, instr.flags.end_moved, state,
                            staged.span, out);
    return;
  }
  const bool is_record_call =
      instr.op == ir::Opcode::Call && !operands.empty() &&
      builder.state().operands[operands.head()].is<ir::ParametricCalleeIdx>();
  if (is_record_call) {
    const ir::Operand head = builder.state().operands[operands.head()];
    const ir::ParametricCallee row =
        builder.state().parametric_callees[head.as_parametric_callee()];
    if (row.kind == ir::ParametricCalleeKind::Dispatch &&
        row.args.size() == 1) {
      const ir::TypeIdx receiver =
          substitute_type(row.args.head(), *state.args);
      const std::optional<BuiltinOperator> builtin = builtin_operator(
          strings.get(row.spec), strings.get(row.method), tag_of(receiver));
      if (builtin.has_value()) {
        staged.op = builtin->op;
        staged.measure = ir::TypeIdx::invalid();
        const bool comparison = comparison_opcode(staged.op);
        for (u32 k = 1; k < operands.size(); ++k) {
          ir::Operand operand =
              translate_operand(builder.state().operands[operands.head() + k]);
          if (failed) {
            return;
          }
          const ir::TypeTag tag = tag_of(operand.type);
          if (!comparison ||
              (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef)) {
            staged.operands.push_back(std::move(operand));
            continue;
          }
          // The method's `&Self` operand: the builtin compares the
          // borrowed values, so the reference is read first.
          const ir::TypeIdx pointee =
              builder.state()
                  .ref_types[builder.state().types[operand.type].as_ref()]
                  .pointee;
          const ir::RegisterIdx value = builder.reg(ir::Register{
              .type = pointee, .def_idx = ir::InstructionIdx::invalid()});
          StagedInstruction load;
          load.op = ir::Opcode::Load;
          load.dst = value;
          load.measure = ir::TypeIdx::invalid();
          load.span = staged.span;
          load.operands.push_back(std::move(operand));
          out.push_back(std::move(load));
          staged.operands.push_back(ir::Operand::from_register(value, pointee));
        }
        if (builtin->zero_first_operand) {
          // `-x` reads as the subtraction the concrete body writes: a
          // zero of the receiver's type minus the value.
          staged.operands.insert(
              staged.operands.begin(),
              ir::Operand::from_immutable(builder.immutable(immutable_from_u64(
                                              tag_of(receiver), receiver, 0)),
                                          receiver));
        }
        out.push_back(std::move(staged));
        return;
      }
    }
    const ir::FunctionIdx callee =
        resolve_callee(head.as_parametric_callee(), state.args, staged.span);
    if (failed) {
      return;
    }
    staged.operands.push_back(ir::Operand::from_function(
        callee, builder.primitive(ir::TypeTag::Function)));
    std::vector<ir::Operand> staged_args;
    for (u32 k = 1; k < operands.size(); ++k) {
      staged_args.push_back(
          translate_operand(builder.state().operands[operands.head() + k]));
    }
    if (failed) {
      return;
    }
    for (ir::Operand& arg : staged_args) {
      staged.operands.push_back(std::move(arg));
    }
    out.push_back(std::move(staged));
    return;
  }
  for (u32 k = 0; k < operands.size(); ++k) {
    staged.operands.push_back(
        translate_operand(builder.state().operands[operands.head() + k]));
  }
  if (failed) {
    return;
  }
  out.push_back(std::move(staged));
}

void Lowerer::resolve_parametric_drop(const ir::Operand& place,
                                      ir::TypeIdx dropped,
                                      bool end_moved,
                                      CloneState& state,
                                      diag::Span span,
                                      std::vector<StagedInstruction>& out) {
  const auto emit_staged = [&](ir::Opcode op, ir::RegisterIdx dst,
                               std::vector<ir::Operand> operands) {
    StagedInstruction staged;
    staged.op = op;
    staged.dst = dst;
    staged.measure = ir::TypeIdx::invalid();
    staged.span = span;
    staged.operands = std::move(operands);
    out.push_back(std::move(staged));
  };
  // The value's type rides on the instruction: the place may be an
  // address (an explicit end), whose own type is a reference.
  const ir::TypeIdx value = dropped.is_valid() ? dropped : place.type;
  const ir::TypeIdx type = substitute_type(value, *state.args);
  if (failed) {
    return;
  }
  // A summary type's destructor body does not cross with the type
  // (ADR-0085): the end cannot run it here, and skipping it would
  // leak, so the build stops and names what did not cross.
  const ir::TypeIdx marker = canonical_type(type);
  if (marker.idx < summary_drops_.size() && summary_drops_[marker.idx] != 0) {
    summary_drop_not_carried(span, type);
    return;
  }
  if (!runs_destructor(type)) {
    return;
  }
  // The end is the drop pass's own walk, emitted once per type as a
  // function that takes the value: a clone's blocks are fixed before
  // its body is translated, so the walk cannot be emitted in place, and
  // one spelling keeps the glue call and the field walk together
  // (ADR-0079).
  const ir::FunctionIdx drop_fn = drop_fn_for(type);
  if (!drop_fn.is_valid()) {
    failed = true;
    return;
  }
  // The parametric body took the place; the concrete end reads the
  // value and marks a binding's place moved.
  const ir::RegisterIdx loaded = builder.reg(
      ir::Register{.type = type, .def_idx = ir::InstructionIdx::invalid()});
  emit_staged(ir::Opcode::Load, loaded, {place});
  const bool movable = ir::is_movable_type(
      builder.state(), type, pkg.not_movable,
      [this](ir::TypeIdx idx) { return canonical_type(idx); });
  if (end_moved && movable) {
    emit_staged(ir::Opcode::Move, ir::RegisterIdx::invalid(), {place});
  }
  emit_staged(ir::Opcode::Call, ir::RegisterIdx::invalid(),
              {ir::Operand::from_function(
                   drop_fn, builder.primitive(ir::TypeTag::Function)),
               ir::Operand::from_register(loaded, type)});
}

void Lowerer::clone_body(ir::FunctionIdx source_idx,
                         ir::FunctionIdx clone,
                         const std::vector<ir::TypeIdx>& args,
                         diag::Span instantiation_span) {
  const ir::Function source = builder.state().functions[source_idx];
  CloneState state;
  state.args = &args;
  state.instantiation_span = instantiation_span;
  // The clone keeps the address names its allocas had, so the borrow
  // pass can still name the places it reports.
  for (const LoweredPackage::AddrInfo& info : addr_names_) {
    state.addr_names.emplace(info.addr.idx, info);
  }
  // Reserve every block first: a nested specialization appends its own
  // while this body is translated, and the clone's range must stay
  // contiguous.
  std::vector<ir::BlockIdx> blocks;
  blocks.reserve(source.blocks.size());
  for (u32 i = 0; i < source.blocks.size(); ++i) {
    blocks.push_back(
        builder.block(ir::Block{.instrs = {}, .block_params = {}}));
  }
  for (u32 i = 0; i < source.blocks.size(); ++i) {
    state.blocks.emplace(static_cast<u32>(source.blocks.head().idx + i),
                         blocks[i]);
  }
  // Block parameters first: a branch names them, and their rows stay
  // contiguous per block.
  std::vector<ir::BlockParamIdxRange> param_ranges(blocks.size());
  for (u32 i = 0; i < source.blocks.size(); ++i) {
    const ir::Block block = builder.state().blocks[source.blocks[i]];
    ir::BlockParamSeq seq;
    for (const ir::BlockParamIdx pidx : block.block_params) {
      const ir::BlockParam param = builder.state().block_params[pidx];
      const ir::TypeIdx type = substitute_type(param.type, args);
      const ir::RegisterIdx reg = clone_register(param.reg, type, state);
      seq.push(builder.block_param(ir::BlockParam{.type = type, .reg = reg}));
    }
    param_ranges[i] = seq.finish();
  }
  for (u32 i = 0; i < source.blocks.size(); ++i) {
    const ir::Block block = builder.state().blocks[source.blocks[i]];
    std::vector<StagedInstruction> staged;
    for (const ir::InstructionIdx iidx : block.instrs) {
      translate_instruction(iidx, state, staged);
      if (failed) {
        return;
      }
    }
    ir::InstrSeq instrs;
    for (StagedInstruction& instruction : staged) {
      const ir::OperandIdx head =
          ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
      for (ir::Operand& operand : instruction.operands) {
        builder.operand(std::move(operand));
      }
      const ir::OperandIdxRange range{
          head, static_cast<u32>(instruction.operands.size())};
      const ir::InstructionIdx created =
          builder.instr(ir::Instruction{.op = instruction.op,
                                        .flags = instruction.flags,
                                        .dst = instruction.dst,
                                        .measure = instruction.measure,
                                        .operands = range});
      instrs.push(created);
      instr_spans_.push_back(instruction.span);
    }
    builder.set_block_instrs(blocks[i], instrs.finish());
    builder.set_block_params(blocks[i], param_ranges[i]);
  }
  builder.set_function_blocks(
      clone,
      ir::BlockIdxRange{blocks.front(), static_cast<u32>(blocks.size())});
}

ir::FunctionIdx Lowerer::method_function(
    u32 module,
    const analyzer::CheckedModule::MethodInfo& info,
    diag::Span span) {
  if (!info.item.is_valid()) {
    internal(span, "a dispatch names an implementation without an item");
    return ir::FunctionIdx::invalid();
  }
  if (parametric_fn_by_item_.find(info.item.idx) !=
      parametric_fn_by_item_.end()) {
    std::vector<ir::TypeIdx> args = receiver_type_args(info.self_type);
    for (const ir::TypeIdx arg : info.method_args) {
      args.push_back(arg);
    }
    return specialize_generic(info.item, args, span);
  }
  const u32 inst = info.method_inst != analyzer::NO_INST
                       ? info.method_inst
                       : generic_inst_index(info.self_type);
  const ir::SymbolKind kind =
      info.receiver == analyzer::CheckedModule::ReceiverKind::None
          ? ir::SymbolKind::Assoc
          : ir::SymbolKind::Method;
  return fn_index(module, info.item, info.name, info.params, info.ret, inst, {},
                  kind, info.method_args, info.self_type);
}

ir::FunctionIdx Lowerer::resolve_dispatch(str::StringPoolId spec,
                                          str::StringPoolId method,
                                          ir::TypeIdx receiver,
                                          diag::Span span) {
  const std::string_view spec_name = strings.get(spec);
  u32 spec_index = U32_MAX;
  for (u32 i = 0; i < static_cast<u32>(pkg.specs.size()); ++i) {
    if (pkg.specs[i].name == spec_name) {
      spec_index = i;
      break;
    }
  }
  if (spec_index == U32_MAX) {
    internal(span, "a dispatch names an unknown spec");
    return ir::FunctionIdx::invalid();
  }
  const std::string_view method_name = strings.get(method);
  // A nominal's name, so a declaration can stand for an instantiation
  // no call checked (ADR-0079).
  const auto nominal_name = [&](ir::TypeIdx type) -> str::StringPoolId {
    const ir::TypeIdx at = canonical_type(type);
    if (!at.is_valid() || at.idx >= builder.state().types.size()) {
      return str::INVALID_STRING_POOL_ID;
    }
    const ir::TypeNode& node = builder.state().types[at];
    if (node.tag == ir::TypeTag::Struct) {
      return builder.state().struct_types[node.as_struct()].name;
    }
    if (node.tag == ir::TypeTag::Enum) {
      return builder.state().enum_types[node.as_enum()].name;
    }
    return str::INVALID_STRING_POOL_ID;
  };
  const str::StringPoolId receiver_name = nominal_name(receiver);
  const analyzer::CheckedModule::MethodInfo* declaration = nullptr;
  for (u32 m = 0; m < static_cast<u32>(pkg.modules.size()); ++m) {
    for (const analyzer::CheckedModule::MethodInfo& info :
         pkg.modules[m].methods) {
      if (info.spec != spec_index || info.name != method_name) {
        continue;
      }
      if (type_equivalent(info.self_type, receiver)) {
        return method_function(m, info, span);
      }
      // The definition-site record: its body is cloned under the
      // receiver's own arguments.
      if (declaration == nullptr &&
          ir::type_names_param(builder.state(), info.self_type) &&
          nominal_name(info.self_type) != str::INVALID_STRING_POOL_ID &&
          nominal_name(info.self_type) == receiver_name) {
        declaration = &info;
      }
    }
  }
  if (declaration != nullptr && declaration->item.is_valid()) {
    std::vector<ir::TypeIdx> args = receiver_type_args(receiver);
    for (const ir::TypeIdx arg : declaration->method_args) {
      args.push_back(arg);
    }
    return specialize_generic(declaration->item, args, span);
  }
  // A generic impl registers no signature until a call names it: the
  // spec table names the impl, and the checked body hangs on its method
  // item (ADR-0079).
  if (receiver_name != str::INVALID_STRING_POOL_ID) {
    const std::string_view receiver_spelling = strings.get(receiver_name);
    for (const auto& impl : pkg.spec_impls) {
      if (impl.spec != spec_index || !impl.item.is_valid()) {
        continue;
      }
      const ast::ItemNode& node = ast.items[impl.item];
      if (node.kind != ast::ItemKind::Impl) {
        continue;
      }
      const ast::ItemImpl& shape = node.payload.get<ast::ItemImpl>();
      if (!shape.type.is_valid() ||
          ast.types[shape.type].kind != ast::TypeKind::Path) {
        continue;
      }
      const ast::TypePath& target =
          ast.types[shape.type].payload.get<ast::TypePath>();
      const std::span<const ast::Ident> segments =
          ast.paths[target.path].segments;
      if (segments.empty() || segments.back().name != receiver_spelling) {
        continue;
      }
      for (const ast::ItemIdx method_item : shape.methods) {
        const ast::ItemNode& method_node = ast.items[method_item];
        if (method_node.kind != ast::ItemKind::Fn) {
          continue;
        }
        if (method_node.payload.get<ast::ItemFn>().name.name != method_name) {
          continue;
        }
        std::vector<ir::TypeIdx> args = receiver_type_args(receiver);
        return specialize_generic(method_item, args, span);
      }
    }
  }
  // A builtin convertible satisfies `Display` without an implementation
  // (ADR-0053): the dispatch answers with the inline expansion for the
  // type, synthesized once (ADR-0079).
  if (spec_name == "Display" && method_name == "display" &&
      analyzer::is_formattable_tag(tag_of(receiver))) {
    return display_fn_for(receiver);
  }
  internal(span, "a dispatch finds no implementation");
  return ir::FunctionIdx::invalid();
}

ir::FunctionIdx Lowerer::resolve_callee(ir::ParametricCalleeIdx callee,
                                        const std::vector<ir::TypeIdx>* subst,
                                        diag::Span span) {
  const ir::ParametricCallee row = builder.state().parametric_callees[callee];
  std::vector<ir::TypeIdx> args;
  args.reserve(row.args.size());
  for (const ir::TypeIdx arg : row.args) {
    args.push_back(subst != nullptr ? substitute_type(arg, *subst) : arg);
  }
  if (row.kind == ir::ParametricCalleeKind::Generic) {
    if (!row.function.is_valid()) {
      internal(span, "a generic callee names no body");
      return ir::FunctionIdx::invalid();
    }
    return specialize_body(row.function, args, span);
  }
  if (args.size() != 1) {
    internal(span, "a dispatch names one receiver");
    return ir::FunctionIdx::invalid();
  }
  return resolve_dispatch(row.spec, row.method, args[0], span);
}

void Lowerer::resolve_record_call(ir::InstructionIdx instr) {
  const ir::Operand head =
      builder.state().operands[builder.state().instrs[instr].operands.head()];
  const diag::Span span =
      instr.idx < instr_spans_.size() ? instr_spans_[instr.idx] : diag::Span{};
  const ir::FunctionIdx callee =
      resolve_callee(head.as_parametric_callee(), nullptr, span);
  if (!callee.is_valid() || failed) {
    failed = true;
    return;
  }
  const ir::Instruction original = builder.state().instrs[instr];
  const u32 count = original.operands.size();
  const ir::OperandIdx rewrite_head =
      ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
  builder.operand(ir::Operand::from_function(
      callee, builder.primitive(ir::TypeTag::Function)));
  for (u32 k = 1; k < count; ++k) {
    builder.operand(
        ir::Operand{builder.state().operands[original.operands.head() + k]});
  }
  builder.set_instruction_operands(instr,
                                   ir::OperandIdxRange{rewrite_head, count});
}

void Lowerer::specialize() {
  if (failed) {
    return;
  }
  if (!specialize_indexes_ready_) {
    for (const ir::FunctionIdx function :
         builder.state().parametric_functions) {
      parametric_functions_.insert(function.idx);
    }
    for (const auto& [type, info] : structs_) {
      struct_instances_[info->item.idx].emplace_back(type);
    }
    for (const auto& [type, info] : enums_) {
      enum_instances_[info->item.idx].emplace_back(type);
    }
    specialize_indexes_ready_ = true;
  }
  // Scan every non-parametric body for records; a resolved call no
  // longer matches, so a repeat sweep is a no-op for it. Every value is
  // copied while the resolve may append to the tables it came from.
  const usize function_count = builder.state().functions.size();
  for (usize f = 0; f < function_count && !failed; ++f) {
    if (parametric_functions_.count(static_cast<u32>(f)) != 0) {
      continue;
    }
    const ir::Function function = builder.state().functions[ir::FunctionIdx(f)];
    for (const ir::BlockIdx block : function.blocks) {
      std::vector<ir::InstructionIdx> list;
      for (const ir::InstructionIdx iidx :
           builder.state().blocks[block].instrs) {
        list.push_back(iidx);
      }
      for (const ir::InstructionIdx iidx : list) {
        const ir::Instruction instr = builder.state().instrs[iidx];
        if (instr.op != ir::Opcode::Call || instr.operands.empty()) {
          continue;
        }
        const ir::Operand head =
            builder.state().operands[instr.operands.head()];
        if (head.is<ir::ParametricCalleeIdx>()) {
          resolve_record_call(iidx);
        }
      }
    }
  }
}

}  // namespace lowering
