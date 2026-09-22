// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace analyzer {

// Name namespaces. Structs and enums occupy Type and Value alike
// (the type and its constructors); functions, statics, and constants
// occupy Value; submodules occupy Module.
enum class Namespace : u8 { Type, Value, Module };

// A resolved import: `name` in this module refers to `member` of the
// module at `target_module`. Re-export chains are already chased to
// their final target.
struct Import {
  std::string_view name;
  Namespace ns;
  u32 target_module;
  std::string_view member;
  bool is_pub;
};

struct ModuleNode {
  // Dotted path from the root ("foo::bar"); "" for the root itself.
  std::string path;
  // kUnknownFile for inline modules, which have no file of their own.
  source::FileId file;
  // Top-level items of this module's file (checked later).
  std::span<ast::Item* const> items;
  std::span<ModuleNode* const> children;
  std::span<const Import> imports;
};

struct ModuleTree {
  std::span<ModuleNode* const> modules;
  u32 root;
};

// Builds the module tree for one package and resolves its imports.
// `root` is the package entry file; `modules` assigns every source
// file its slash-separated module name ("" names the root itself).
// Every file is lexed, parsed, and desugared here (each in a
// per-file arena). Module membership comes from the caller, never
// from source items, so no new files enter the compilation. Value
// and type expressions are NOT resolved here; that is later
// semantic work over ModuleNode::items.
struct ModuleInput {
  std::string_view name;
  source::FileId id = source::kUnknownFile;
};

diag::Fallible<ModuleTree> resolve_modules(source::FileId root,
                                           std::span<const ModuleInput> modules,
                                           std::string_view package_name,
                                           source::SourceManager& sources,
                                           mem::Arena& arena,
                                           diag::DiagBag& bag);

}  // namespace analyzer
