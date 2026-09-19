// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace analyzer {

// Per-module type information for Phase B and lowering. All TypeIdx
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
  struct FnSig {
    std::string_view name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret;
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
  };
  std::vector<MethodInfo> methods;
  struct StaticInfo {
    std::string_view name;
    ir::TypeIdx type;
  };
  std::vector<StaticInfo> statics;
};

struct CheckedPackage {
  ModuleTree tree;
  ir::Storage types;
  // Aligned with tree.modules by index.
  std::vector<CheckedModule> modules;
  // Every blessed instantiation in the package, in first-use order.
  // Expression checking (Phase B4) maps a TypeIdx here for `?`,
  // construction, and must_use; identity is the interned index.
  struct BlessedType {
    bool is_result;
    ir::TypeIdx type;
    // [T, E] for Result, [T] for Option.
    std::vector<ir::TypeIdx> args;
  };
  std::vector<BlessedType> blessed;
};

// Resolves every type position in the package to interned TypeIdx:
// nominal definitions (structs, enums), signatures, and blessed
// instantiations. Reports unknown, duplicate, reserved, recursive,
// and malformed types. Expressions are untouched; that is Phase B.
diag::Fallible<CheckedPackage> check_package(const ModuleTree& tree,
                                             ir::PointerWidth width,
                                             diag::DiagBag& bag);

}  // namespace analyzer
