// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "source/source.h"

namespace analyzer {

// A module tree handed to check_package failed structural verification.

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

// The module index that names no module. Zero is the entry module, so the
// sentinel has to sit outside the index space.
constexpr u32 NO_MODULE = std::numeric_limits<u32>::max();

// The package-root index that names no package root. Module roots sit in
// their own index space, so this sentinel is distinct from NO_MODULE's
// role even though the two share a value.
constexpr u32 NO_PACKAGE_ROOT = std::numeric_limits<u32>::max();

// One package root the tree carries besides its own: a staged
// standard-library member or a path dependency, staged behind a
// fileless root module. `identity` is the name a `use` or a
// qualified path spells; `module` is the fileless root it opens.
// A path dependency is trimmed to its `[modules] export` list, so
// `exports` names the modules it makes public; a staged
// standard-library member opens through its facade, so nothing
// trims it. Views borrow the caller's storage.
struct PackageRoot {
  std::string_view identity;
  u32 module = NO_MODULE;
  bool trimmed = false;
  std::span<const std::string_view> exports;
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
  // they are toolchain sources rather than the program's own. Path
  // dependencies are not staged, so their modules are counted like the
  // package's own.
  u32 staged_modules = 0;
  // The package roots besides the package's own: staged
  // standard-library members and path dependencies, each behind a
  // fileless root a `use` or a qualified path spells the identity of.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::span<const PackageRoot> package_roots = {};
  // The package root each module belongs to, or NO_PACKAGE_ROOT for
  // the package's own modules and the staged standard library's. A
  // `use` from inside a package stays inside it, so the export list
  // does not trim it. Empty in a hand-built tree, which reads as no
  // module belonging to a package root.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::span<const u32> module_roots = {};

  // The package root `module` belongs to, or NO_PACKAGE_ROOT. Trees
  // built by resolve_modules carry one entry per module; a hand-built
  // tree carries none, which reads as no dependency.
  [[nodiscard]] u32 module_root(u32 module) const {
    return module_roots.empty() ? NO_PACKAGE_ROOT : module_roots[module];
  }

  // Whether `item` is declared by the staged package `package`. Only the
  // embedded standard library is staged, so the staged flag plus a path
  // check is exactly package identity for now; registry packages will
  // need a real one.
  //
  // Compiler-known expansions key on this rather than on a name, so a
  // user function called `write` stays ordinary. See
  // docs/adr/0016-suites-and-the-std-split.md and
  // docs/adr/0025-ranges-as-data.md.
  [[nodiscard]] bool is_staged_item(std::string_view package,
                                    ast::ItemIdx item) const;
};

// Whether `path` names the package `package` or a module inside it.
inline bool is_package_path(std::string_view path, std::string_view package) {
  return path == package || (path.size() > package.size() + 1 &&
                             path.compare(0, package.size(), package) == 0 &&
                             path.compare(package.size(), 2, "::") == 0);
}

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
  // `module_roots` is neither empty nor one per module.
  BadModuleRootCount,
};

// Pure structural verification of a module tree: non-empty, a root in
// range, no null modules, a prelude count within range, and package
// roots that name modules the tree holds. Trees built by
// resolve_modules satisfy this by construction; hand-built trees must
// pass before crossing into check_package. No I/O, no logging, no bag
// writes.
base::Result<void, ModuleTreeError> verify_module_tree(const ModuleTree& tree);

// The `core` package of the staged standard library: the `core` root
// and everything under it. The interval types are declared there and
// their names are reserved, so a range expression always constructs
// the one declaration. See docs/adr/0025-ranges-as-data.md.
inline bool is_core_package(std::string_view path) {
  return is_package_path(path, "core");
}

// Short human-readable detail for a module tree failure.
std::string_view describe_module_tree_error(ModuleTreeError error);

// A module's identity for resolution: the name a `use` path spells, the
// file the items came from, and whether its public surface is a
// facade's. The items come alongside it, in `ParsedModule`.
struct ModuleInput {
  std::string_view name;
  source::FileId id = source::UNKNOWN_FILE;
  // A facade's public surface is in scope without a `use`. Set by
  // std_prelude for a staged `prelude.al`, never from a user's module
  // name.
  bool is_facade = false;
};

// One module input and the syntax the parser made of its file. Lexing,
// parsing, and desugaring happen before this, in `pipeline`; resolution
// starts from the items, so it never reads a source byte and never
// depends on how many threads produced them. `path` is the file's
// canonical spelling, kept for the file that turns out to belong to no
// module. The items borrow the run's arena; the path is owned here, and
// both outlive the resolution that reads them.
struct ParsedModule {
  ModuleInput input;
  path::Path path;
  std::span<const ast::ItemIdx> items;
};

// One public name of one embedded suite member, for the
// missing-dependency hint. Views borrow the generated tables, so no
// arena is involved.
struct StdHint {
  std::string_view package;
  std::string_view name;
};

// One path dependency's modules, staged behind the package root its
// manifest name gives. `identity` is the name a `use` spells: the
// manifest name with `-` normalized to `_`. `exports` is the
// dependency's `[modules] export` list, the only surface a `use`
// from outside the package reaches. `modules` are the dependency's
// selected modules, named by their paths within the package, and the
// resolver stages them behind a fileless root named by the identity,
// the way staged standard-library sources sit behind their suite
// roots - but as ordinary modules, since a dependency's surface is
// its export list, never an implicit prelude. Views borrow the
// caller's storage.
struct DependencyPackage {
  std::string_view identity;
  std::span<const std::string_view> exports;
  std::span<const ParsedModule> modules;
};

// Names the embedded member carrying `name` - a public item of it,
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
template <diag::DiagnosticId Id>
inline void emit_unresolved(diag::DiagBag& bag,
                            diag::Stage stage,
                            Id id,
                            diag::Span span,
                            std::span<const StdHint> hints,
                            std::string_view kind,
                            std::string_view name) {
  const std::string_view package = std_hint_package(hints, name);
  if (!package.empty()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUnresolvedInStandardLibrary>(
        diag::Severity::Error, stage, id, span, kind, name, name, package);
    (void)index;
    return;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
      diag::Severity::Error, stage, id, span, kind, name);
  (void)index;
}

// Builds the module tree for one package and resolves its imports.
// `root` is the package entry file; `modules` assigns every source
// file its slash-separated module name ("" names the root itself) and
// carries the items parsing produced for it. Module membership comes
// from the caller, never from source items, so no new files enter the
// compilation. Value and type expressions are NOT resolved here; that
// is later semantic work over ModuleNode::items.
//
// `prelude` lists additional source files resolved as standalone
// modules outside the package tree. Every facade's public items are
// implicitly imported by every other module (locals and explicit uses
// win silently), which is where the toolchain's core sources attach.
//
// `dependencies` lists the path dependencies the package loads from
// source, each staged behind a fileless package root of its own. A
// `use` or a qualified path spells a dependency by its identity, and
// reaches only its `[modules] export` list.
//
// The caller owns the items, the paths, and the arena they live in,
// and keeps all three alive for the call. `pipeline::parse_files`
// produces exactly this input.
base::Result<ModuleTree, diag::Reported> resolve_modules(
    source::FileId root,
    std::span<const ParsedModule> modules,
    std::string_view package_name,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    std::span<const ParsedModule> prelude = {},
    std::span<const StdHint> std_hints = {},
    std::span<const DependencyPackage> dependencies = {});

// A `use` or a qualified path that reaches `module_path` in the
// package the root `root` opens, seen from `from_module`. A path
// from inside the package stays inside it, so only one from outside
// is trimmed to the export list, and this reports the module the
// package keeps to itself. Returns whether the boundary withheld the
// module.
bool emit_withheld_module(std::span<const PackageRoot> roots,
                          std::span<const u32> module_roots,
                          u32 from_module,
                          u32 root,
                          std::string_view module_path,
                          diag::Span span,
                          diag::DiagBag& bag);

}  // namespace analyzer
