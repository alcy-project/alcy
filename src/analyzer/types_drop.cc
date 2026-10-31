// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/diag_code.h"
#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/verifier.h"

namespace analyzer {

bool Checker::check_drop_signature(u32 module,
                                   ir::TypeIdx self_type,
                                   const CheckedModule::FnSig& sig,
                                   CheckedModule::ReceiverKind receiver,
                                   ast::ItemIdx item) {
  static constexpr std::string_view DROP = "drop";
  if (sig.name != DROP || receiver == CheckedModule::ReceiverKind::None) {
    return false;
  }
  const ast::Ident& name = ast.items[item].payload.get<ast::ItemFn>().name;
  const bool valid = receiver == CheckedModule::ReceiverKind::ByValue &&
                     sig.params.size() == 1 &&
                     tag_of(sig.ret) == ir::TypeTag::Void;
  if (!valid) {
    const u32 index = bag.emit<i18n::Key::AnalyzerBadDestructorSignature>(
        DiagCode::BadDropSignature, name.span);
    (void)index;
    return false;
  }
  const ir::TypeTag self_tag = tag_of(self_type);
  const bool view_like =
      self_tag == ir::TypeTag::Str || self_tag == ir::TypeTag::Slice ||
      self_tag == ir::TypeTag::Ref || self_tag == ir::TypeTag::MutRef ||
      self_tag == ir::TypeTag::RawPtr || self_tag == ir::TypeTag::RawMutPtr ||
      self_tag == ir::TypeTag::Func || self_tag == ir::TypeTag::Function;
  if (view_like) {
    // A view is a window on storage someone else owns; it has nothing
    // to end, so a destructor on it is refused. Any other type may
    // declare one, and the declaration makes it move-only.
    const u32 index = bag.emit<i18n::Key::AnalyzerViewCannotHaveDestructor>(
        DiagCode::DropOnCopy, name.span);
    (void)index;
    return false;
  }
  (void)module;
  return true;
}

void Checker::resolve_drops() {
  drop_glue_.clear();
  needs_drop_.clear();
  std::vector<u32> stack;
  // Resolving a generic destructor instantiates it, which can intern
  // further types, so the scan follows the type table as it grows.
  for (u32 i = 0; i < builder.types().size(); ++i) {
    drop_scan(ir::TypeIdx(i), stack);
  }
}

bool Checker::drop_scan(ir::TypeIdx type, std::vector<u32>& stack) {
  if (needs_drop_.size() <= type.idx) {
    drop_glue_.resize(type.idx + 1, CheckedModule::DropGlue{});
    needs_drop_.resize(type.idx + 1, 0);
  }
  if (needs_drop_[type.idx]) {
    return true;
  }
  // A struct's field range holds storage copies so it stays contiguous,
  // and a copy is a distinct type index. Destructors are declared on the
  // type the copy came from, so the scan follows the copy back.
  const ir::TypeIdx origin = type_origin(type);
  if (origin.idx != type.idx) {
    const bool result = drop_scan(origin, stack);
    drop_glue_[type.idx] = drop_glue_[origin.idx];
    needs_drop_[type.idx] = result;
    return result;
  }
  for (u32 entry : stack) {
    // A type that contains itself has no finite destructor walk. The
    // cycle check rejects such a type before this runs.
    if (entry == type.idx) {
      return false;
    }
  }
  stack.push_back(type.idx);
  const ir::TypeNode node = builder.types()[type];
  bool result = false;
  CheckedModule::DropGlue glue;
  switch (node.tag) {
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: {
      glue = find_drop_glue(type);
      if (glue.index != base::INVALID_IDX) {
        result = true;
        break;
      }
      // No destructor of its own: ending a value still runs the
      // destructors of whatever it holds.
      result = holds_destructible(type, stack);
      break;
    }
    case ir::TypeTag::Array:
      result = drop_scan(builder.array_types()[node.as_array()].element, stack);
      break;
    case ir::TypeTag::Tuple: {
      const ir::TupleType tuple = builder.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (drop_scan(element, stack)) {
          result = true;
          break;
        }
      }
      break;
    }
    default: break;
  }
  stack.pop_back();
  needs_drop_[type.idx] = result;
  if (glue.index != base::INVALID_IDX) {
    drop_glue_[type.idx] = glue;
  }
  return result;
}

CheckedModule::MethodInfo& Checker::add_method(u32 module,
                                               CheckedModule::MethodInfo info) {
  CheckedModule& target = modules[module];
  target.methods.push_back(std::move(info));
  const CheckedModule::MethodInfo& method = target.methods.back();
  const u32 position = static_cast<u32>(target.methods.size() - 1);
  methods_by_self_[method.self_type.idx].emplace_back(module, position);
  position_by_address_.emplace(&method, std::pair<u32, u32>{module, position});
  if (!method.is_drop) {
    return target.methods.back();
  }
  const CheckedModule::DropGlue glue{module, position};
  const u32 key = method.self_type.idx;
  const auto found = drop_glue_by_type_.find(key);
  if (found == drop_glue_by_type_.end() || found->second.module > glue.module ||
      (found->second.module == glue.module &&
       found->second.index > glue.index)) {
    drop_glue_by_type_[key] = glue;
  }
  return target.methods.back();
}

// The nominal shape a type node names: two nodes over one struct or
// enum are the same declaration however they were interned.

std::optional<u64> Checker::drop_shape_key(ir::TypeIdx type) const {
  const ir::TypeNode node = builder.types()[type];
  if (node.tag == ir::TypeTag::Struct) {
    return (static_cast<u64>(ir::TypeTag::Struct) << 32) | node.as_struct().idx;
  }
  if (node.tag == ir::TypeTag::Enum) {
    return (static_cast<u64>(ir::TypeTag::Enum) << 32) | node.as_enum().idx;
  }
  return std::nullopt;
}

CheckedModule::DropGlue Checker::find_drop_glue(ir::TypeIdx type) {
  const auto found = drop_glue_by_type_.find(type.idx);
  if (found != drop_glue_by_type_.end()) {
    if (const std::optional<u64> shape = drop_shape_key(type);
        shape.has_value()) {
      drop_glue_by_shape_.emplace(*shape, found->second);
    }
    return found->second;
  }
  // A second type node over one nominal shape is the same declaration,
  // so the destructor the shape already answered with answers for it
  // too: a node interned through another path carries no glue of its
  // own, and a value of either ends the same way.
  const std::optional<u64> shape = drop_shape_key(type);
  if (shape.has_value()) {
    const auto shaped = drop_glue_by_shape_.find(*shape);
    if (shaped != drop_glue_by_shape_.end()) {
      return shaped->second;
    }
  }
  // A generic type reaches its destructor through `impl<T> Name<T>`,
  // which only exists once instantiated.
  const CheckedModule::MethodInfo* method =
      lookup_method(type, "drop", NO_MODULE, diag::Span{}, false);
  if (method == nullptr) {
    return {};
  }
  // Where the method lives is what the address table holds. This walked every
  // method in the package to find the one it already had, once per type asked
  // about, so a package paid its methods for each of its types.
  const auto position = position_by_address_.find(method);
  if (position == position_by_address_.end()) {
    return {};
  }
  const CheckedModule::DropGlue glue{position->second.first,
                                     position->second.second};
  if (shape.has_value()) {
    drop_glue_by_shape_.emplace(*shape, glue);
  }
  return glue;
}

bool Checker::holds_destructible(ir::TypeIdx type, std::vector<u32>& stack) {
  const ir::TypeNode node = builder.types()[type];
  if (node.tag == ir::TypeTag::Struct) {
    // The walk below builds glue, which can move the table this shape
    // was read from, so the shape is a copy and not a reference.
    const ir::StructType shape = builder.struct_types()[node.as_struct()];
    for (ir::TypeIdx field : shape.fields) {
      if (drop_scan(field, stack)) {
        return true;
      }
    }
    return false;
  }
  const ir::EnumType shape = builder.enum_types()[node.as_enum()];
  for (ir::EnumVariantTypeIdx v : shape.variants) {
    const ir::EnumVariantType variant = builder.enum_variant_types()[v];
    for (ir::TypeIdx payload : variant.fields) {
      if (drop_scan(payload, stack)) {
        return true;
      }
    }
  }
  return false;
}

bool Checker::has_value_cycle(ir::TypeIdx root,
                              std::vector<ir::TypeIdx>& stack,
                              const ir::Storage& storage) {
  for (ir::TypeIdx entry : stack) {
    if (entry.idx == root.idx) {
      return true;
    }
  }
  const ir::TypeNode& node = storage.types()[root];
  stack.push_back(root);
  bool cycle = false;
  switch (node.tag) {
    case ir::TypeTag::Struct: {
      const ir::StructType struct_type =
          storage.struct_types()[node.as_struct()];
      for (ir::TypeIdx field : struct_type.fields) {
        if (has_value_cycle(field, stack, storage)) {
          cycle = true;
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Enum: {
      const ir::EnumType& enum_type = storage.enum_types()[node.as_enum()];
      for (ir::EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = ir::EnumVariantTypeIdx(vidx.idx + 1)) {
        const ir::EnumVariantType variant = storage.enum_variant_types()[vidx];
        for (ir::TypeIdx field : variant.fields) {
          if (has_value_cycle(field, stack, storage)) {
            cycle = true;
            break;
          }
        }
        if (cycle) {
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType tuple = storage.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (has_value_cycle(element, stack, storage)) {
          cycle = true;
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType array = storage.array_types()[node.as_array()];
      cycle = has_value_cycle(array.element, stack, storage);
      break;
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType slice = storage.slice_types()[node.as_slice()];
      cycle = has_value_cycle(slice.element, stack, storage);
      break;
    }
    default: break;
  }
  stack.pop_back();
  return cycle;
}

}  // namespace analyzer
