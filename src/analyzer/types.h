// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace analyzer {

// Per-module type information for type checking and lowering. All TypeIdx
// refer to the package Storage below; all views borrow source bytes.
struct CheckedModule {
  u32 module;

  struct NamedType {
    std::string_view name;
    ir::TypeIdx type;
  };
  std::vector<NamedType> types;
  // Field names parallel to ir::StructType field lists, per struct.
  struct StructInfo {
    ir::TypeIdx type;
    std::vector<std::string_view> fields;
  };
  std::vector<StructInfo> structs;
  // Variant names in declaration order (the index is the discriminant),
  // mirroring structs for matches and construction.
  struct EnumInfo {
    std::string_view name;
    ir::TypeIdx type;
    std::vector<std::string_view> variants;
  };
  std::vector<EnumInfo> enums;
  struct FnSig {
    std::string_view name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret;
    // Declaring item for body lowering (invalid for synthesized entries).
    ast::ItemIdx item;
  };
  std::vector<FnSig> functions;
  // Inherent methods per impl block, including associated functions
  // (receiver None). Mirrors the resolved signatures above so call
  // checking can match (self type, name) without re-walking AST.
  enum class ReceiverKind : u8 { None, ByValue, Shared, Exclusive };
  struct MethodInfo {
    ir::TypeIdx self_type;
    std::string_view name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret;
    ReceiverKind receiver;
    ast::ItemIdx item;
  };
  std::vector<MethodInfo> methods;
  struct StaticInfo {
    std::string_view name;
    ir::TypeIdx type;
    // Initializer for const inlining (invalid for statics in lowering).
    ast::ExprIdx init;
    bool is_const = false;
  };
  std::vector<StaticInfo> statics;
  // Lowering side tables: every checked expression records its type,
  // and every checked call records its callee, so lowering never
  // re-resolves paths or re-derives types.
  std::vector<std::pair<ast::ExprIdx, ir::TypeIdx>> expr_types;
  struct CallTarget {
    ast::ExprIdx callee;
    bool is_method;
    u32 module;
    u32 index;
  };
  std::vector<CallTarget> call_targets;
  // Variant resolution for lowering: every checked variant use records
  // its meaning so lowering never re-resolves paths. `variant` is the
  // declaration-order index for user enums; blessed constructors use
  // `blessed_first` (Ok/Some side) with the instantiation in `enum_type`.
  struct VariantUse {
    ast::PathIdx path;
    bool blessed = false;
    bool blessed_first = true;
    ir::TypeIdx enum_type;
    u32 variant = 0;
  };
  std::vector<VariantUse> variants;
};

struct CheckedPackage {
  ModuleTree tree;
  ir::Storage types;
  // Aligned with tree.modules by index.
  std::vector<CheckedModule> modules;
  // Every blessed instantiation in the package, in first-use order.
  // Expression checking maps a TypeIdx here for `?`, construction,
  // and must_use; identity is the interned index.
  struct BlessedType {
    bool is_result;
    ir::TypeIdx type;
    // [T, E] for Result, [T] for Optional.
    std::vector<ir::TypeIdx> args;
  };
  std::vector<BlessedType> blessed;
};

// Resolves every type position in the package to interned TypeIdx:
// nominal definitions (structs, enums), signatures, and blessed
// instantiations. Reports unknown, duplicate, reserved, recursive,
// and malformed types, then checks bodies.
diag::Fallible<CheckedPackage> check_package(const ModuleTree& tree,
                                             ir::PointerWidth width,
                                             ast::AstArena& ast,
                                             diag::DiagBag& bag);

}  // namespace analyzer
