// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "analyzer/resolve.h"

#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/desugar.h"
#include "parser/parser.h"
#include "path/path.h"
#include "source/source.h"

namespace analyzer {

namespace {

// Diagnostic codes 4200-4299 are reserved for module resolution.
constexpr u32 kAnalyzerDuplicateModule = 4201;
constexpr u32 kAnalyzerUnresolvedImport = 4202;
constexpr u32 kAnalyzerAmbiguousImport = 4203;
constexpr u32 kAnalyzerUnreachableFile = 4204;
constexpr u32 kAnalyzerInvalidPath = 4205;

constexpr u32 kNoModule = std::numeric_limits<u32>::max();

struct FileData {
  source::FileId id = source::kUnknownFile;
  path::Path path;
  // Items are parsed into the caller-provided arena (never a per-file
  // arena): ModuleNode::items outlives resolve_modules, so per-file
  // arenas would dangle.
  std::span<ast::Item* const> items;
  u32 module = kNoModule;

  FileData(source::FileId id, path::Path path)
      : id(id), path(std::move(path)) {}
};

struct NameEntry {
  std::string_view name;
};

struct Resolver {
  Resolver(source::SourceManager& sources,
           mem::Arena& arena,
           diag::DiagBag& bag)
      : sources(sources), arena(arena), bag(bag) {}

  source::SourceManager& sources;
  mem::Arena& arena;
  diag::DiagBag& bag;
  std::string_view package_name;
  source::FileId root = source::kUnknownFile;
  std::vector<FileData> file_data;
  std::vector<ModuleNode*> modules;
  std::vector<u32> parents;
  std::vector<std::vector<NameEntry>> local_types;
  std::vector<std::vector<NameEntry>> local_values;
  std::vector<std::vector<NameEntry>> local_modules;
  std::vector<std::vector<Import>> module_imports;
  std::vector<std::vector<u32>> module_children;
  // Export resolution state per module: 0 fresh, 1 in progress, 2 done.
  std::vector<u8> exports_state;

  u32 add_module(std::string path,
                 source::FileId file,
                 std::span<ast::Item* const> items,
                 u32 parent) {
    ModuleNode* node = arena.create<ModuleNode>();
    node->path = std::move(path);
    node->file = file;
    node->items = items;
    modules.push_back(node);
    parents.push_back(parent);
    local_types.emplace_back();
    local_values.emplace_back();
    local_modules.emplace_back();
    module_imports.emplace_back();
    module_children.emplace_back();
    exports_state.push_back(0);
    return static_cast<u32>(modules.size() - 1);
  }

  bool has_name(const std::vector<NameEntry>& entries,
                std::string_view name) const {
    for (const NameEntry& entry : entries) {
      if (entry.name == name) {
        return true;
      }
    }
    return false;
  }

  std::string_view module_name(u32 module) const {
    const std::string& path = modules[module]->path;
    const usize slash = path.find_last_of(':');
    if (slash == std::string::npos) {
      return std::string_view(path);
    }
    return std::string_view(path).substr(slash + 1);
  }

  u32 find_child_module(u32 module, std::string_view name) const {
    for (u32 child : module_children[module]) {
      if (module_name(child) == name) {
        return child;
      }
    }
    return kNoModule;
  }

  void lex_parse_file(FileData& file) {
    const std::string_view bytes = sources.bytes(file.id);
    lexer::Lexer lexer(bytes, file.id, bag);
    std::vector<lexer::Token> tokens;
    lexer.tokenize(tokens);
    parser::Parser parser(
        std::span<const lexer::Token>(tokens.data(), tokens.size()), bytes,
        file.id, arena, bag);
    file.items = parser.parse();
    parser::desugar_shadowing(file.items, arena, bag);
  }

  void build_tree() {
    u32 root_file = kNoModule;
    for (u32 i = 0; i < static_cast<u32>(file_data.size()); ++i) {
      if (file_data[i].id == root) {
        root_file = i;
      }
    }
    DCHECK(root_file != kNoModule);
    const u32 root_module =
        add_module("", root, file_data[root_file].items, kNoModule);
    file_data[root_file].module = root_module;

    for (u32 i = 0; i < static_cast<u32>(file_data.size()); ++i) {
      if (i == root_file) {
        continue;
      }
      attach_module(root_module, module_inputs[i], i);
    }

    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      std::vector<ModuleNode*> children;
      children.reserve(module_children[m].size());
      for (u32 child : module_children[m]) {
        children.push_back(modules[child]);
      }
      modules[m]->children = ast::copy_to_arena(arena, children);
    }

    for (const FileData& file : file_data) {
      if (file.module == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Warning, kAnalyzerUnreachableFile,
                     "source file '{}' is not reachable from the package root",
                     file.path.as_view());
        (void)index;
      }
    }
  }

  // Attaches one listed file under the root, creating fileless
  // intermediate nodes for slash-separated names (`utils/io`
  // becomes root → `utils` → `utils::io`).
  void attach_module(u32 root_module, std::string_view slash_name, u32 file) {
    u32 parent = root_module;
    std::string prefix;
    usize start = 0;
    while (start <= slash_name.size()) {
      usize slash = slash_name.find('/', start);
      if (slash == std::string_view::npos) {
        slash = slash_name.size();
      }
      const std::string_view segment = slash_name.substr(start, slash - start);
      const std::string child_path = prefix.empty()
                                         ? std::string(segment)
                                         : prefix + "::" + std::string(segment);
      const bool leaf = slash == slash_name.size();
      u32 child = find_child_module(parent, segment);
      if (child == kNoModule) {
        child = add_module(
            child_path, leaf ? file_data[file].id : source::kUnknownFile,
            leaf ? file_data[file].items : std::span<ast::Item* const>{},
            parent);
        module_children[parent].push_back(child);
      } else if (leaf) {
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerDuplicateModule, diag::Span{},
            "module '{}' is declared more than once", slash_name);
        (void)index;
        return;
      }
      parent = child;
      prefix = child_path;
      start = slash + 1;
    }
    file_data[file].module = parent;
  }

  void collect_locals() {
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      for (ast::Item* item : modules[m]->items) {
        switch (item->kind) {
          case ast::ItemKind::Struct:
          case ast::ItemKind::Enum: {
            std::string_view name{};
            if (item->kind == ast::ItemKind::Struct) {
              name = static_cast<const ast::StructItem*>(item)->name.name;
            } else {
              name = static_cast<const ast::EnumItem*>(item)->name.name;
            }
            local_types[m].push_back(NameEntry{name});
            local_values[m].push_back(NameEntry{name});
            break;
          }
          case ast::ItemKind::Fn:
          case ast::ItemKind::Static:
          case ast::ItemKind::Const: {
            std::string_view name{};
            if (item->kind == ast::ItemKind::Fn) {
              name = static_cast<const ast::FnItem*>(item)->name.name;
            } else if (item->kind == ast::ItemKind::Static) {
              name = static_cast<const ast::StaticItem*>(item)->name.name;
            } else {
              name = static_cast<const ast::ConstItem*>(item)->name.name;
            }
            local_values[m].push_back(NameEntry{name});
            break;
          }
          case ast::ItemKind::Use:
          case ast::ItemKind::Impl: break;
        }
      }
      for (u32 child : module_children[m]) {
        local_modules[m].push_back(NameEntry{module_name(child)});
      }
    }
  }

  // Resolves one module's imports, chasing re-exports recursively.
  // Public uses resolve first so later uses (including `self::` ones)
  // observe a complete export set regardless of declaration order.
  void resolve_exports(u32 module) {
    if (exports_state[module] == 2) {
      return;
    }
    if (exports_state[module] == 1) {
      const u32 index = bag.emit(
          diag::Severity::Error, kAnalyzerUnresolvedImport,
          modules[module]->items.empty() ? diag::Span{}
                                         : modules[module]->items[0]->span,
          "dependency cycle in re-exports");
      (void)index;
      exports_state[module] = 2;
      return;
    }
    exports_state[module] = 1;
    for (ast::Item* item : modules[module]->items) {
      if (item->kind != ast::ItemKind::Use) {
        continue;
      }
      const ast::UseItem* use = static_cast<const ast::UseItem*>(item);
      if (use->is_pub) {
        resolve_use(module, use);
      }
    }
    for (ast::Item* item : modules[module]->items) {
      if (item->kind != ast::ItemKind::Use) {
        continue;
      }
      const ast::UseItem* use = static_cast<const ast::UseItem*>(item);
      if (!use->is_pub) {
        resolve_use(module, use);
      }
    }
    exports_state[module] = 2;
  }

  void add_import(u32 module,
                  const ast::UseItem* use,
                  std::string_view name,
                  Namespace ns,
                  u32 target,
                  std::string_view member) {
    if (has_name(ns == Namespace::Type    ? local_types[module]
                 : ns == Namespace::Value ? local_values[module]
                                          : local_modules[module],
                 name)) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerAmbiguousImport, use->span,
                   "`{}` conflicts with a local item", name);
      (void)index;
      return;
    }
    for (const Import& prior : module_imports[module]) {
      if (prior.ns == ns && prior.name == name) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerAmbiguousImport, use->span,
                     "`{}` is imported more than once", name);
        (void)index;
        return;
      }
    }
    module_imports[module].push_back(
        Import{name, ns, target, member, use->is_pub});
  }

  // Looks a member up in a module's locals.
  bool lookup_local(u32 module, std::string_view member, Namespace ns) const {
    return has_name(ns == Namespace::Type    ? local_types[module]
                    : ns == Namespace::Value ? local_values[module]
                                             : local_modules[module],
                    member);
  }

  // Looks a member up in a module's public imports (re-exports).
  bool lookup_reexport(u32 module,
                       std::string_view member,
                       Namespace ns,
                       u32& target_out,
                       std::string_view& member_out) const {
    for (const Import& import : module_imports[module]) {
      if (import.is_pub && import.ns == ns && import.name == member) {
        target_out = import.target_module;
        member_out = import.member;
        return true;
      }
    }
    return false;
  }

  void resolve_use(u32 module, const ast::UseItem* use) {
    std::vector<std::string_view> segments;
    for (const ast::Ident& segment : use->path->segments) {
      segments.push_back(segment.name);
    }
    if (segments.size() < 2) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnresolvedImport, use->span,
                   "imports must be module-qualified (`self::foo`)");
      (void)index;
      return;
    }
    u32 current = kNoModule;
    const std::string_view head = segments[0];
    if (head == "package" || head == package_name) {
      current = 0;
      for (u32 i = 0; i < static_cast<u32>(modules.size()); ++i) {
        if (modules[i]->path.empty()) {
          current = i;
          break;
        }
      }
    } else if (head == "self") {
      current = module;
    } else if (head == "super") {
      if (parents[module] == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnresolvedImport,
                     use->span, "the root module has no parent");
        (void)index;
        return;
      }
      current = parents[module];
    } else {
      current = find_child_module(module, head);
      if (current == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnresolvedImport,
                     use->span, "unresolved import '{}'", head);
        (void)index;
        return;
      }
    }
    for (usize i = 1; i + 1 < segments.size(); ++i) {
      current = find_child_module(current, segments[i]);
      if (current == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnresolvedImport,
                     use->span, "unresolved import '{}'", segments[i]);
        (void)index;
        return;
      }
    }
    const std::string_view member = segments.back();
    const std::string_view name = use->has_alias ? use->alias.name : member;
    // Locals first: no recursion is needed and self-targets never false
    // cycle. Re-exports follow only for members locals lack.
    bool resolved = false;
    u32 target = kNoModule;
    std::string_view final_member;
    // A struct or enum name occupies the type and value namespaces alike.
    const Namespace namespaces[] = {Namespace::Module, Namespace::Type,
                                    Namespace::Value};
    for (Namespace ns : namespaces) {
      if (lookup_local(current, member, ns)) {
        add_import(module, use, name, ns, current, member);
        resolved = true;
      }
    }
    if (!resolved) {
      if (current != module) {
        resolve_exports(current);
      }
      for (Namespace ns : namespaces) {
        if (lookup_reexport(current, member, ns, target, final_member)) {
          add_import(module, use, name, ns, target, final_member);
          resolved = true;
        }
      }
    }
    if (!resolved) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnresolvedImport, use->span,
                   "unresolved import '{}'", member);
      (void)index;
    }
  }

  std::vector<std::string> module_inputs;

  ModuleTree run(source::FileId root_id,
                 std::span<const ModuleInput> inputs,
                 std::string_view package_name_in) {
    package_name = package_name_in;
    root = root_id;
    file_data.reserve(inputs.size());
    for (const ModuleInput& input : inputs) {
      base::Result<path::Path, path::PathError> canonical =
          path::Path::from_native(sources.name(input.id));
      if (canonical.is_err()) {
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidPath,
                                   "invalid source path for file");
        (void)index;
        continue;
      }
      path::Path path = std::move(canonical).unwrap();
      module_inputs.emplace_back(input.name);
      file_data.emplace_back(input.id, std::move(path));
    }
    for (FileData& file : file_data) {
      lex_parse_file(file);
    }
    build_tree();
    collect_locals();
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      resolve_exports(m);
    }
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      modules[m]->imports = ast::copy_to_arena(arena, module_imports[m]);
    }
    u32 root_index = 0;
    for (u32 i = 0; i < static_cast<u32>(modules.size()); ++i) {
      if (modules[i]->path.empty()) {
        root_index = i;
        break;
      }
    }
    ModuleTree tree;
    tree.modules = ast::copy_to_arena(
        arena, std::vector<ModuleNode*>(modules.begin(), modules.end()));
    tree.root = root_index;
    return tree;
  }
};

}  // namespace

diag::Fallible<ModuleTree> resolve_modules(
    source::FileId root,
    std::span<const ModuleInput> modules,
    std::string_view package_name,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag) {
  Resolver resolver{sources, arena, bag};
  return base::make_ok(resolver.run(root, modules, package_name));
}

}  // namespace analyzer
