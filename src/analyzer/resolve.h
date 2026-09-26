// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "source/source.h"

namespace analyzer {

// A module tree handed to check_package failed structural verification.
inline constexpr u32 ANALYZER_INVALID_MODULE_TREE = 4005;

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
  // UNKNOWN_FILE for inline modules, which have no file of their own.
  source::FileId file;
  // Top-level items of this module's file (checked later).
  std::span<const ast::ItemIdx> items;
  std::span<ModuleNode* const> children;
  std::span<const Import> imports;
  // Toolchain prelude sources; user modules never set this.
  bool is_prelude = false;
};

struct ModuleTree {
  std::span<ModuleNode* const> modules;
  u32 root;
  // Standalone prelude modules appended after package modules.
  u32 prelude_modules = 0;
};

// Structural failure of a module tree handed to check_package.
enum class ModuleTreeError : u8 {
  // No modules at all.
  Empty,
  // `root` names no module.
  RootOutOfRange,
  // A module entry is null.
  NullModule,
  // `prelude_modules` exceeds the module count.
  BadPreludeCount,
};

// Pure structural verification of a module tree: non-empty, a root in
// range, no null modules, and a prelude count within range. Trees
// built by resolve_modules satisfy this by construction; hand-built
// trees must pass before crossing into check_package. No I/O, no
// logging, no bag writes.
base::Result<void, ModuleTreeError> verify_module_tree(const ModuleTree& tree);

// Short human-readable detail for a module tree failure.
std::string_view describe_module_tree_error(ModuleTreeError error);

// Builds the module tree for one package and resolves its imports.
// `root` is the package entry file; `modules` assigns every source
// file its slash-separated module name ("" names the root itself).
// Every file is lexed, parsed, and desugared here (each in a
// per-file arena). Module membership comes from the caller, never
// from source items, so no new files enter the compilation. Value
// and type expressions are NOT resolved here; that is later
// semantic work over ModuleNode::items.
//
// `prelude` lists additional source files resolved as standalone
// modules outside the package tree. Every other module implicitly
// imports their public items (locals and explicit uses win
// silently); this is where toolchain-provided core sources will
// attach once they exist.
struct ModuleInput {
  std::string_view name;
  source::FileId id = source::UNKNOWN_FILE;
};

base::Result<ModuleTree, diag::Reported> resolve_modules(
    source::FileId root,
    std::span<const ModuleInput> modules,
    std::string_view package_name,
    source::SourceManager& sources,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    std::span<const ModuleInput> prelude = {});

}  // namespace analyzer
