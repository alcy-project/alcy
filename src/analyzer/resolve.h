// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
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
  // Any module the staged standard library added: facades, their
  // siblings, and the fileless package roots between them. A facade is
  // the subset whose public surface is in scope without a `use`; the
  // rest are ordinary modules that happen to be toolchain sources.
  bool is_staged = false;
};

struct ModuleTree {
  std::span<ModuleNode* const> modules;
  u32 root;
  // Count of facade modules appended after the package's own: one per
  // staged prelude package, whose public surface is in scope without a
  // `use`.
  u32 prelude_modules = 0;
  // Every module the staged prelude added, facades and the fileless
  // package roots between them. Reported counts exclude these, since
  // they are toolchain sources rather than the program's own.
  u32 staged_modules = 0;
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

// The `fmt` package of the staged standard library: the `fmt` root
// and everything under it. Compiler-known `write`/`format` expansions
// key on this rather than on a name, so a user function called `write`
// stays ordinary. See docs/adr/0016.
inline bool is_fmt_package(std::string_view path) {
  return path == "fmt" || (path.size() > 5 && path.substr(0, 5) == "fmt::");
}

// Whether `item` is declared by the staged `fmt` package. Only the
// embedded standard library is staged, so a path check plus the staged
// flag is exactly package identity for now; registry packages will need
// a real one.
inline bool is_fmt_item(const ModuleTree& tree, ast::ItemIdx item) {
  for (const ModuleNode* module : tree.modules) {
    if (!module->is_staged || !is_fmt_package(module->path)) {
      continue;
    }
    for (ast::ItemIdx candidate : module->items) {
      if (candidate == item) {
        return true;
      }
    }
  }
  return false;
}

// Short human-readable detail for a module tree failure.
std::string_view describe_module_tree_error(ModuleTreeError error);

// Builds the module tree for one package and resolves its imports.
// `root` is the package entry file; `modules` assigns every source
// file its slash-separated module name ("" names the root itself).
// Every file is lexed, parsed, and desugared here. Module membership
// comes from the caller, never from source items, so no new files enter
// the compilation. Value and type expressions are NOT resolved here;
// that is later semantic work over ModuleNode::items.
//
// `prelude` lists additional source files resolved as standalone
// modules outside the package tree. Every facade's public items are
// implicitly imported by every other module (locals and explicit uses
// win silently), which is where the toolchain's core sources attach.
struct ModuleInput {
  std::string_view name;
  source::FileId id = source::UNKNOWN_FILE;
  // A facade's public surface is in scope without a `use`. Set by
  // std_prelude for a staged `prelude.al`, never from a user's module
  // name.
  bool is_facade = false;
};

// One public name of one embedded suite member, for the
// missing-dependency hint. Views borrow the generated tables, so no
// arena is involved.
struct StdHint {
  std::string_view package;
  std::string_view name;
};

// Names the embedded member carrying `name` — a public item of it,
// or the member itself. Empty when the name is not standard library.
inline std::string_view std_hint_package(std::span<const StdHint> hints,
                                         std::string_view name) {
  for (const StdHint& hint : hints) {
    if (hint.name == name || hint.package == name) {
      return hint.package;
    }
  }
  return {};
}

// Emits an unresolved-name error, naming the embedded member when the
// name is a public item of one. The hint tells a manifest without the
// package exactly what to add; anything else reads the plain message.
inline void emit_unresolved(diag::DiagBag& bag,
                            u32 code,
                            diag::Span span,
                            std::span<const StdHint> hints,
                            std::string_view kind,
                            std::string_view name) {
  const std::string_view package = std_hint_package(hints, name);
  if (!package.empty()) {
    const u32 index = bag.emit(diag::Severity::Error, code, span,
                               "unresolved {} '{}'; `{}` is in "
                               "the standard library "
                               "(alcy/std/{}); add it to "
                               "[dependencies]",
                               kind, name, name, package);
    (void)index;
    return;
  }
  const u32 index = bag.emit(diag::Severity::Error, code, span,
                             "unresolved {} '{}'", kind, name);
  (void)index;
}

base::Result<ModuleTree, diag::Reported> resolve_modules(
    source::FileId root,
    std::span<const ModuleInput> modules,
    std::string_view package_name,
    source::SourceManager& sources,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    std::span<const ModuleInput> prelude = {},
    std::span<const StdHint> std_hints = {});

}  // namespace analyzer
