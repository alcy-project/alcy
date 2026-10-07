// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/resolve.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/diag_code.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "source/source.h"

namespace analyzer {

namespace {

// One module's file, as `resolve_modules` received it: the name the
// caller assigned, the syntax parsing produced, and the canonical path
// the file is reported by. Every view borrows the caller's storage, so
// nothing here outlives the call.
struct FileData {
  source::FileId id = source::UNKNOWN_FILE;
  // Slash-separated module name; "" is the entry module.
  std::string_view name;
  // Canonical file path, named when a file belongs to no module.
  std::string_view path;
  std::span<const ast::ItemIdx> items;
  // A facade's public surface attaches without a `use`. Prelude only.
  bool is_facade = false;
  u32 module = NO_MODULE;
};

struct NameEntry {
  std::string_view name;
};

class Resolver {
 public:
  Resolver(ast::AstArena& ast, diag::DiagBag& bag, debug::Profiler* profiler)
      : ast(ast), bag(bag), profiler(profiler) {}

  ast::AstArena& ast;
  diag::DiagBag& bag;
  // Where the walk's scopes go, or nothing.
  debug::Profiler* profiler = nullptr;
  std::string_view package_name;
  source::FileId root = source::UNKNOWN_FILE;
  std::vector<FileData> file_data;
  // Prelude sources: attached as their own tree, never into the package
  // tree. A facade is the root of one prelude package; the rest of that
  // package's modules sit beside it and are ordinary modules.
  std::span<const StdHint> std_hints_;
  std::vector<FileData> prelude_data;
  std::vector<u32> prelude_modules;
  std::vector<ModuleNode*> modules;
  std::vector<u32> parents;
  std::vector<std::vector<NameEntry>> local_types;
  std::vector<std::vector<NameEntry>> local_values;
  std::vector<std::vector<NameEntry>> local_modules;
  std::vector<std::vector<Import>> module_imports;
  std::vector<std::vector<u32>> module_children;
  // The same children, by the name a path segment spells. Attaching a file
  // found its child by walking the siblings and comparing names, and the entry
  // module has every module as a child, so a package paid its modules for each
  // one. The first child of a name is the one the walk would have found.
  std::vector<std::unordered_map<std::string_view, u32>> children_by_name_;
  // Export resolution state per module: 0 fresh, 1 in progress, 2 done.
  std::vector<u8> exports_state;
  // The package roots this tree carries besides its own, and the
  // root each module belongs to. Both grow with the modules they
  // name, and both index the same spaces ModuleTree::package_roots
  // and ModuleTree::module_roots describe.
  std::vector<PackageRoot> package_roots_;
  std::vector<u32> module_roots_;
  // The caller's spec policies, held so the tree's arena copy can be
  // built after every package root exists.
  std::vector<PackagePolicy> package_policies_;
  // Set when a node could not be placed, so the tree was left unfinished.
  bool out_of_arena = false;

  // The one refusal resolve raises on its own: the syntax arena could not
  // hold the tree's own nodes, so there is no tree to hand back.
  void fail_out_of_arena() {
    if (out_of_arena) {
      return;
    }
    out_of_arena = true;
    const u32 index = bag.emit<i18n::Key::AnalyzerArenaExhausted>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArenaExhausted,
        diag::Span{}, ast.spans.capacity());
    (void)index;
  }

  u32 add_module(std::string path,
                 source::FileId file,
                 std::span<const ast::ItemIdx> items,
                 u32 parent) {
    ModuleNode* node = ast.spans.create<ModuleNode>();
    if (node == nullptr) {
      fail_out_of_arena();
      return NO_MODULE;
    }
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
    children_by_name_.emplace_back();
    exports_state.push_back(0);
    module_roots_.push_back(NO_PACKAGE_ROOT);
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
    const auto found = children_by_name_[module].find(name);
    return found == children_by_name_[module].end() ? NO_MODULE : found->second;
  }

  void build_tree() {
    u32 root_file = NO_MODULE;
    for (u32 i = 0; i < static_cast<u32>(file_data.size()); ++i) {
      if (file_data[i].id == root) {
        root_file = i;
      }
    }
    if (root_file == NO_MODULE) {
      // The root file was not among the inputs, so there is nothing to
      // attach a tree to. A checked index would read out of bounds.
      const u32 index = bag.emit<i18n::Key::AnalyzerRootFileNotAModuleInput>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidPath,
          diag::Span{});
      (void)index;
      return;
    }
    const u32 root_module =
        add_module("", root, file_data[root_file].items, NO_MODULE);
    if (out_of_arena) {
      return;
    }
    file_data[root_file].module = root_module;

    for (u32 i = 0; i < static_cast<u32>(file_data.size()); ++i) {
      if (i == root_file) {
        continue;
      }
      attach_module(root_module, file_data[i].name, i);
      if (out_of_arena) {
        return;
      }
    }

    for (const FileData& file : file_data) {
      if (file.module == NO_MODULE) {
        const u32 index = bag.emit<i18n::Key::AnalyzerSourceFileHasNoModule>(
            diag::Severity::Warning, diag::Stage::Analyzer,
            DiagCode::UnreachableFile, file.path);
        (void)index;
      }
    }
  }

  // Splits a slash-separated module name into segments, dropping a
  // trailing `.al` so a staged source path names the same module as the
  // manifest path it came from.
  static std::vector<std::string_view> split_segments(std::string_view name) {
    std::vector<std::string_view> segments;
    usize start = 0;
    while (start <= name.size()) {
      usize slash = name.find('/', start);
      if (slash == std::string_view::npos) {
        slash = name.size();
      }
      std::string_view segment = name.substr(start, slash - start);
      if (segment.ends_with(path::SOURCE_EXTENSION)) {
        segment.remove_suffix(path::SOURCE_EXTENSION.size());
      }
      if (!segment.empty()) {
        segments.push_back(segment);
      }
      start = slash + 1;
    }
    return segments;
  }

  // Attaches one listed file under the root, creating fileless
  // intermediate nodes for slash-separated names (`utils/io`
  // becomes root -> `utils` -> `utils::io`).
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
      if (child == NO_MODULE) {
        child = add_module(
            child_path, leaf ? file_data[file].id : source::UNKNOWN_FILE,
            leaf ? file_data[file].items : std::span<const ast::ItemIdx>{},
            parent);
        if (out_of_arena) {
          return;
        }
        module_children[parent].push_back(child);
        children_by_name_[parent].emplace(module_name(child), child);
      } else if (leaf) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateModule>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateModule, diag::Span{}, slash_name);
        (void)index;
        return;
      }
      parent = child;
      prefix = child_path;
      start = slash + 1;
    }
    file_data[file].module = parent;
  }

  // Attaches one module of a dependency under the fileless root the
  // package opens, nesting slash-separated names the way attach_module
  // nests a package's own files under the entry module. The module
  // belongs to the dependency, so a `use` from inside the package
  // stays inside it and the export list does not trim it.
  void attach_dependency_module(u32 root_module,
                                u32 dependency,
                                std::string_view slash_name,
                                const ParsedModule& input) {
    u32 parent = root_module;
    std::string prefix(std::string(modules[root_module]->path));
    usize start = 0;
    while (start <= slash_name.size()) {
      usize slash = slash_name.find('/', start);
      if (slash == std::string_view::npos) {
        slash = slash_name.size();
      }
      const std::string_view segment = slash_name.substr(start, slash - start);
      const std::string child_path = prefix + "::" + std::string(segment);
      const bool leaf = slash == slash_name.size();
      u32 child = find_child_module(parent, segment);
      if (child == NO_MODULE) {
        child = add_module(
            child_path, leaf ? input.input.id : source::UNKNOWN_FILE,
            leaf ? input.items : std::span<const ast::ItemIdx>{}, parent);
        if (out_of_arena) {
          return;
        }
        module_children[parent].push_back(child);
        children_by_name_[parent].emplace(module_name(child), child);
        module_roots_[child] = dependency;
      } else if (leaf) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateModule>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateModule, diag::Span{}, slash_name);
        (void)index;
        return;
      }
      parent = child;
      prefix = child_path;
      start = slash + 1;
    }
  }

  // Stages the path dependencies behind their fileless package roots,
  // the way the staged standard library sits behind its suite roots,
  // but as ordinary modules: a dependency's surface is its `[modules]
  // export` list, reached by `use`, never an implicit prelude, and
  // nothing reserves its names the way the standard library's do.
  bool stage_dependencies(std::span<const DependencyPackage> dependencies) {
    for (const DependencyPackage& dependency : dependencies) {
      u32 root_module = NO_MODULE;
      for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
        if (modules[m]->path == dependency.identity &&
            parents[m] == NO_MODULE) {
          root_module = m;
          break;
        }
      }
      // The pipeline names a package that would share a root before
      // resolution, so reaching this means a tree was assembled by
      // hand: two packages behind one spelling cannot both be right.
      if (root_module != NO_MODULE) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateModule>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateModule, diag::Span{}, dependency.identity);
        (void)index;
        return false;
      }
      root_module =
          add_module(std::string(dependency.identity), source::UNKNOWN_FILE,
                     std::span<const ast::ItemIdx>{}, NO_MODULE);
      if (out_of_arena) {
        return false;
      }
      const u32 root_index = static_cast<u32>(package_roots_.size());
      package_roots_.push_back(PackageRoot{dependency.identity, root_module,
                                           true, dependency.exports});
      module_roots_[root_module] = root_index;
      for (const ParsedModule& input : dependency.modules) {
        attach_dependency_module(root_module, root_index, input.input.name,
                                 input);
        if (out_of_arena) {
          return false;
        }
      }
    }
    return true;
  }

  void collect_locals() {
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(
          profiler, trace_module_name(*modules[m]), "frontend");
      for (ast::ItemIdx item : modules[m]->items) {
        const ast::ItemNode& node = ast.items[item];
        std::string_view name = node.name();
        using I = ast::ItemKind;
        switch (node.kind) {
          case I::Struct:
          case I::Enum: {
            local_types[m].push_back(NameEntry{name});
            local_values[m].push_back(NameEntry{name});
            break;
          }
          case I::Spec: {
            local_types[m].push_back(NameEntry{name});
            break;
          }
          case I::Fn:
          case I::Intrinsic:
          case I::Static:
          case I::Const: {
            local_values[m].push_back(NameEntry{name});
            break;
          }
          case I::Extern: {
            for (const ast::ItemExternFn& fn :
                 node.payload.get<ast::ItemExtern>().fns) {
              local_values[m].push_back(NameEntry{fn.name.name});
            }
            break;
          }
          case I::Use:
          case I::Impl: break;
        }
      }
      for (u32 child : module_children[m]) {
        local_modules[m].push_back(NameEntry{module_name(child)});
      }
    }
  }

  // Injects implicit imports of every public prelude item into
  // every non-prelude module. Locals and explicit uses win silently;
  // the imports never re-export (is_pub false).
  void inject_prelude() {
    for (u32 prelude : prelude_modules) {
      // A facade's re-exports resolve first: a `pub use` names the
      // surface the facade stands in for, and resolution is idempotent,
      // so the later sweep over every module is unaffected.
      resolve_exports(prelude);
      for (ast::ItemIdx item : modules[prelude]->items) {
        const ast::ItemNode& node = ast.items[item];
        if (!node.is_pub) {
          continue;
        }
        const std::string_view name = node.name();
        using I = ast::ItemKind;
        const Namespace namespaces[] = {Namespace::Type, Namespace::Value,
                                        Namespace::Module};
        for (Namespace ns : namespaces) {
          const bool declared =
              (ns == Namespace::Type &&
               (node.kind == I::Struct || node.kind == I::Enum ||
                node.kind == I::Spec)) ||
              (ns == Namespace::Value &&
               (node.kind == I::Struct || node.kind == I::Enum ||
                node.kind == I::Fn || node.kind == I::Intrinsic ||
                node.kind == I::Static || node.kind == I::Const));
          if (!declared) {
            continue;
          }
          inject_name(name, ns, prelude, name, prelude);
        }
      }
      // A facade re-exports what its siblings declare, so the resolved
      // import rides along: the name stays visible without a `use`, and
      // the target is the declaration itself rather than the facade.
      for (const Import& reexport : module_imports[prelude]) {
        if (!reexport.is_pub) {
          continue;
        }
        inject_name(reexport.name, reexport.ns, reexport.target_module,
                    reexport.member, prelude);
      }
    }
  }

  // Makes `name` visible in every module but `skip`, unless a local or
  // an earlier import already claims it. Locals win over the prelude,
  // and the first prelude to claim a name keeps it.
  void inject_name(std::string_view name,
                   Namespace ns,
                   u32 target,
                   std::string_view member,
                   u32 skip) {
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      if (m == skip) {
        continue;
      }
      const std::vector<NameEntry>& locals =
          ns == Namespace::Type    ? local_types[m]
          : ns == Namespace::Value ? local_values[m]
                                   : local_modules[m];
      if (has_name(locals, name)) {
        continue;
      }
      bool imported = false;
      for (const Import& prior : module_imports[m]) {
        if (prior.ns == ns && prior.name == name) {
          imported = true;
          break;
        }
      }
      if (!imported) {
        module_imports[m].push_back(Import{name, ns, target, member, false});
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
      const u32 index = bag.emit<i18n::Key::PkgDependencyCycleInReExports>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::UnresolvedImport,
          modules[module]->items.empty()
              ? diag::Span{}
              : ast.items[modules[module]->items[0]].span);
      (void)index;
      exports_state[module] = 2;
      return;
    }
    exports_state[module] = 1;

    // Resolve public uses
    for (ast::ItemIdx item : modules[module]->items) {
      if (ast.items[item].kind != ast::ItemKind::Use) {
        continue;
      }
      if (ast.items[item].is_pub) {
        resolve_use(module, item);
      }
    }

    // Resolve private uses
    for (ast::ItemIdx item : modules[module]->items) {
      if (ast.items[item].kind != ast::ItemKind::Use) {
        continue;
      }
      if (!ast.items[item].is_pub) {
        resolve_use(module, item);
      }
    }
    exports_state[module] = 2;
  }

  void add_import(u32 module,
                  ast::ItemIdx use,
                  std::string_view name,
                  Namespace ns,
                  u32 target,
                  std::string_view member) {
    const ast::ItemNode& use_node = ast.items[use];
    if (has_name(ns == Namespace::Type    ? local_types[module]
                 : ns == Namespace::Value ? local_values[module]
                                          : local_modules[module],
                 name)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerConflictsWithLocalItem>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::AmbiguousImport, use_node.span, name);
      (void)index;
      return;
    }
    for (const Import& prior : module_imports[module]) {
      if (prior.ns == ns && prior.name == name) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateImport>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::AmbiguousImport, use_node.span, name);
        (void)index;
        return;
      }
    }
    module_imports[module].push_back(
        Import{name, ns, target, member, use_node.is_pub});
  }

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

  void resolve_use(u32 module, ast::ItemIdx use) {
    const ast::ItemNode& node = ast.items[use];
    const ast::Path& path = ast.paths[node.payload.get<ast::ItemUse>().path];
    std::vector<std::string_view> segments;
    for (const ast::Ident& segment : path.segments) {
      segments.push_back(segment.name);
    }
    if (segments.size() < 2) {
      const u32 index = bag.emit<i18n::Key::AnalyzerImportNotQualified>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::UnresolvedImport, node.span);
      (void)index;
      return;
    }
    u32 current = NO_MODULE;
    const std::string_view head = segments[0];
    // The head opens a package when it spells one of the tree's
    // package roots: the staged standard library's members and the
    // path dependencies. A package root wins over a module of the
    // same name, so a dependency is reached by its identity and a
    // same-named module by `package::` or `self::`.
    u32 opened_root = NO_PACKAGE_ROOT;
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
      if (parents[module] == NO_MODULE) {
        const u32 index = bag.emit<i18n::Key::AnalyzerRootModuleHasNoParent>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::UnresolvedImport, node.span);
        (void)index;
        return;
      }
      current = parents[module];
    } else {
      for (u32 i = 0; i < static_cast<u32>(package_roots_.size()); ++i) {
        if (package_roots_[i].identity == head) {
          current = package_roots_[i].module;
          opened_root = i;
          break;
        }
      }
      if (opened_root == NO_PACKAGE_ROOT) {
        current = find_child_module(module, head);
      }
      if (current == NO_MODULE) {
        emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnresolvedImport,
                        node.span, std_hints_, "import", head);
        return;
      }
    }
    for (usize i = 1; i + 1 < segments.size(); ++i) {
      current = find_child_module(current, segments[i]);
      if (current == NO_MODULE) {
        emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnresolvedImport,
                        node.span, std_hints_, "import", segments[i]);
        return;
      }
    }
    const std::string_view member = segments.back();
    // A use that crosses into a dependency reaches only that
    // package's `[modules] export` list. The module the member
    // lives in is the walk's result; a member that is itself a
    // module is imported by its own path, so the list names the
    // module the use imports rather than the one it names it from.
    // A use from inside the dependency stays inside it.
    if (opened_root != NO_PACKAGE_ROOT) {
      std::string module_path;
      for (usize i = 1; i + 1 < segments.size(); ++i) {
        if (!module_path.empty()) {
          module_path.push_back('/');
        }
        module_path.append(segments[i]);
      }
      if (lookup_local(current, member, Namespace::Module)) {
        if (module_path.empty()) {
          module_path = std::string(member);
        } else {
          module_path.push_back('/');
          module_path.append(member);
        }
      }
      if (!module_path.empty() &&
          emit_withheld_module(package_roots_, module_roots_, module,
                               opened_root, module_path, node.span, bag)) {
        return;
      }
    }
    const std::string_view name =
        node.payload.get<ast::ItemUse>().has_alias
            ? node.payload.get<ast::ItemUse>().alias.name
            : member;
    // Locals first: that needs no recursion, and a self-target cannot
    // cycle. Re-exports follow only for members locals lack.
    bool resolved = false;
    u32 target = NO_MODULE;
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
      emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnresolvedImport,
                      node.span, std_hints_, "import", member);
    }
  }

  ModuleTree run(source::FileId root_id,
                 std::span<const ParsedModule> inputs,
                 std::string_view package_name_in,
                 std::span<const ParsedModule> prelude,
                 std::span<const StdHint> std_hints,
                 std::span<const DependencyPackage> dependencies,
                 std::span<const PackagePolicy> package_policies) {
    package_name = package_name_in;
    root = root_id;
    std_hints_ = std_hints;
    package_policies_.assign(package_policies.begin(), package_policies.end());
    file_data.reserve(inputs.size());
    for (const ParsedModule& input : inputs) {
      file_data.push_back({input.input.id, input.input.name,
                           input.path.as_view(), input.items});
    }
    prelude_data.reserve(prelude.size());
    for (const ParsedModule& input : prelude) {
      prelude_data.push_back({input.input.id, input.input.name,
                              input.path.as_view(), input.items,
                              input.input.is_facade});
    }
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "build-tree",
                                               "frontend");
      build_tree();
    }
    if (out_of_arena) {
      return ModuleTree{};
    }
    // A prelude package is a tree: slash-separated names nest, so
    // `core/prelude.al` and `core/mem.al` share a fileless `core` root
    // and can reach each other. Only a facade is a prelude, so only its
    // public surface is in scope without a `use`.
    const u32 modules_before = static_cast<u32>(modules.size());
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "stage-prelude",
                                               "frontend");
      for (FileData& file : prelude_data) {
        const std::vector<std::string_view> segments =
            split_segments(file.name);
        u32 parent = NO_MODULE;
        std::string prefix;
        for (usize seg = 0; seg < segments.size(); ++seg) {
          const bool leaf = seg + 1 == segments.size();
          const std::string child_path =
              prefix.empty() ? std::string(segments[seg])
                             : prefix + "::" + std::string(segments[seg]);
          u32 child = NO_MODULE;
          for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
            if (modules[m]->path == child_path && parents[m] == parent) {
              child = m;
              break;
            }
          }
          if (child == NO_MODULE) {
            child = add_module(
                child_path, leaf ? file.id : source::UNKNOWN_FILE,
                leaf ? file.items : std::span<const ast::ItemIdx>{}, parent);
            if (out_of_arena) {
              return ModuleTree{};
            }
            // The whole staged tree is toolchain sources, facades and
            // siblings alike; only facades are preludes.
            modules[child]->is_staged = true;
          } else if (leaf) {
            // Two staged sources resolved to the same leaf: the later one
            // would silently overwrite the earlier module's items.
            const u32 index =
                bag.emit<i18n::Key::AnalyzerDuplicatePreludeModule>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::DuplicateModule,
                    file.id == source::UNKNOWN_FILE ? diag::Span{}
                                                    : diag::Span{file.id, 0, 0},
                    child_path);
            (void)index;
            continue;
          }
          // A top-level module has no parent to hang off, so the root
          // case skips the edge entirely.
          if (parent != NO_MODULE) {
            module_children[parent].push_back(child);
            children_by_name_[parent].emplace(module_name(child), child);
          }
          prefix = child_path;
          parent = child;
        }
        file.module = parent;
        if (file.is_facade) {
          modules[parent]->is_prelude = true;
          prelude_modules.push_back(parent);
        }
      }
    }
    // The staged count stops at the standard library: a path
    // dependency's modules are the program's own, counted like any
    // other module rather than read as toolchain sources.
    const u32 staged_modules =
        static_cast<u32>(modules.size()) - modules_before;
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "stage-dependencies",
                                               "frontend");
      if (!stage_dependencies(dependencies)) {
        return ModuleTree{};
      }
    }
    // Every module now exists and every edge is in place, so the
    // children spans are taken here rather than in build_tree: the
    // prelude and the dependencies stage their modules behind
    // fileless roots the package's own files never name, and the
    // checker walks these spans the way resolution walks the edges.
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "link-children",
                                               "frontend");
      for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
        std::vector<ModuleNode*> children;
        children.reserve(module_children[m].size());
        for (u32 child : module_children[m]) {
          children.push_back(modules[child]);
        }
        modules[m]->children = ast::copy_to_arena(ast.spans, children);
        if (ast.exhausted()) {
          fail_out_of_arena();
          return ModuleTree{};
        }
      }
    }
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "collect-locals",
                                               "frontend");
      collect_locals();
    }
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "inject-prelude",
                                               "frontend");
      inject_prelude();
    }
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "resolve-exports",
                                               "frontend");
      for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
        PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(
            profiler, trace_module_name(*modules[m]), "frontend");
        resolve_exports(m);
      }
    }
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "link-imports",
                                               "frontend");
      for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
        modules[m]->imports = ast::copy_to_arena(ast.spans, module_imports[m]);
      }
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
        ast.spans, std::vector<ModuleNode*>(modules.begin(), modules.end()));
    tree.package_roots = ast::copy_to_arena(ast.spans, package_roots_);
    tree.module_roots = ast::copy_to_arena(ast.spans, module_roots_);
    tree.package_policies = ast::copy_to_arena(ast.spans, package_policies_);
    tree.package_name = package_name;
    if (ast.exhausted()) {
      fail_out_of_arena();
      return ModuleTree{};
    }
    tree.root = root_index;
    tree.prelude_modules = static_cast<u32>(prelude_modules.size());
    tree.staged_modules = staged_modules;
    return tree;
  }
};

}  // namespace

bool ModuleTree::is_staged_item(std::string_view package,
                                ast::ItemIdx item) const {
  for (const ModuleNode* module : modules) {
    if (!module->is_staged || !is_package_path(module->path, package)) {
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

base::Result<ModuleTree, diag::Reported> resolve_modules(
    source::FileId root,
    std::span<const ParsedModule> modules,
    std::string_view package_name,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    std::span<const ParsedModule> prelude,
    std::span<const StdHint> std_hints,
    std::span<const DependencyPackage> dependencies,
    std::span<const PackagePolicy> package_policies,
    debug::Profiler* profiler) {
  Resolver resolver{ast, bag, profiler};
  ModuleTree tree = resolver.run(root, modules, package_name, prelude,
                                 std_hints, dependencies, package_policies);
  if (bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(tree);
}

bool emit_withheld_module(std::span<const PackageRoot> roots,
                          std::span<const u32> module_roots,
                          u32 from_module,
                          u32 root,
                          std::string_view module_path,
                          diag::Span span,
                          diag::DiagBag& bag) {
  const PackageRoot& package = roots[root];
  if (!package.trimmed) {
    return false;
  }
  // A path from inside the package stays inside it, where the
  // package's own modules are all visible.
  if (!module_roots.empty() && module_roots[from_module] == root) {
    return false;
  }
  for (std::string_view exported : package.exports) {
    if (exported == module_path) {
      return false;
    }
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerExportWithheld>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ExportWithheld,
      span, package.identity, module_path);
  (void)index;
  return true;
}

base::Result<void, ModuleTreeError> verify_module_tree(const ModuleTree& tree) {
  PROFILE_SCOPE_WITH_CATEGORY("verify-tree", "frontend");
  if (tree.modules.empty()) {
    return base::make_err(ModuleTreeError::Empty);
  }
  if (tree.root >= tree.modules.size()) {
    return base::make_err(ModuleTreeError::RootOutOfRange);
  }
  for (const ModuleNode* module : tree.modules) {
    if (module == nullptr) {
      return base::make_err(ModuleTreeError::NullModule);
    }
  }
  if (tree.prelude_modules > tree.modules.size()) {
    return base::make_err(ModuleTreeError::BadPreludeCount);
  }
  if (!tree.module_roots.empty() &&
      tree.module_roots.size() != tree.modules.size()) {
    return base::make_err(ModuleTreeError::BadModuleRootCount);
  }
  for (const PackageRoot& package_root : tree.package_roots) {
    if (package_root.module >= tree.modules.size()) {
      return base::make_err(ModuleTreeError::RootOutOfRange);
    }
  }
  return base::make_ok();
}

std::string_view describe_module_tree_error(ModuleTreeError error) {
  switch (error) {
    case ModuleTreeError::Empty: return "module tree has no modules";
    case ModuleTreeError::RootOutOfRange:
      return "module tree root names no module";
    case ModuleTreeError::NullModule: return "module tree holds a null module";
    case ModuleTreeError::BadPreludeCount:
      return "module tree prelude count exceeds the module count";
    case ModuleTreeError::BadModuleRootCount:
      return "module tree module roots do not cover the module count";
  }
  return "invalid module tree";
}

}  // namespace analyzer
