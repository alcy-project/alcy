// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/resolve.h"

#include <limits>
#include <optional>
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
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/desugar.h"
#include "parser/parser.h"
#include "path/path.h"
#include "source/source.h"

namespace analyzer {

namespace {

// Diagnostic codes 4000-4009 are reserved for module resolution.
constexpr u32 ANALYZER_DUPLICATE_MODULE = 4000;
constexpr u32 ANALYZER_UNRESOLVED_IMPORT = 4001;
constexpr u32 ANALYZER_AMBIGUOUS_IMPORT = 4002;
constexpr u32 ANALYZER_UNREACHABLE_FILE = 4003;
constexpr u32 ANALYZER_INVALID_PATH = 4004;
// The arena the parser fills reports running out by trapping, so the
// check that catches it first has to live where the reservation does.
constexpr u32 ANALYZER_SPAN_ARENA_EXHAUSTED = 4006;

constexpr u32 NO_MODULE = std::numeric_limits<u32>::max();

struct FileData {
  source::FileId id = source::UNKNOWN_FILE;
  path::Path path;
  // Items are parsed into the package AstArena (never a per-file
  // arena): ModuleNode::items outlives resolve_modules, so per-file
  // arenas would dangle.
  std::span<const ast::ItemIdx> items;
  u32 module = NO_MODULE;

  FileData(source::FileId id, path::Path path)
      : id(id), path(std::move(path)) {}
};

struct NameEntry {
  std::string_view name;
};

class Resolver {
 public:
  Resolver(source::SourceManager& sources,
           ast::AstArena& ast,
           diag::DiagBag& bag)
      : sources(sources), ast(ast), bag(bag) {}

  source::SourceManager& sources;
  ast::AstArena& ast;
  diag::DiagBag& bag;
  std::string_view package_name;
  source::FileId root = source::UNKNOWN_FILE;
  std::vector<FileData> file_data;
  // Prelude sources: lexed and parsed like package files but attached
  // as their own tree, never into the package tree. A facade is the
  // root of one prelude package; the rest of that package's modules sit
  // beside it and are ordinary modules.
  std::span<const StdHint> std_hints_;
  std::vector<FileData> prelude_data;
  std::vector<std::string> prelude_names;
  std::vector<bool> prelude_facades;
  std::vector<u32> prelude_modules;
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
                 std::span<const ast::ItemIdx> items,
                 u32 parent) {
    ModuleNode* node = ast.spans.create<ModuleNode>();
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
    return NO_MODULE;
  }

  // Lexes, parses and desugars one file, reporting whether it did. The
  // arena reports running out by trapping, naming neither the file nor
  // the input, so a file it cannot hold is refused here instead - and it
  // is what the arena has spent that is checked rather than how large
  // the file is, since what a file costs is the identifiers it names.
  bool lex_parse_file(FileData& file) {
    // File ids were validated when the inputs were admitted in run().
    const std::optional<std::string_view> file_bytes = sources.bytes(file.id);
    DCHECK(file_bytes.has_value());
    const std::string_view bytes = file_bytes.value_or(std::string_view{});
    if (ast.spans_nearly_full()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpanArenaExhausted>(
          diag::Severity::Error, ANALYZER_SPAN_ARENA_EXHAUSTED,
          diag::Span{file.id, 0, 0}, ast.spans.capacity());
      (void)index;
      return false;
    }
    lexer::Lexer lexer(bytes, file.id, bag);
    std::vector<lexer::Token> tokens;
    {
      PROFILE_SCOPE_WITH_CATEGORY("tokenize", "frontend");
      lexer.tokenize(tokens);
    }
    parser::Parser parser(
        std::span<const lexer::Token>(tokens.data(), tokens.size()), bytes,
        file.id, ast, bag);
    base::Result<std::span<const ast::ItemIdx>, diag::Reported> parsed = [&] {
      PROFILE_SCOPE_WITH_CATEGORY("parse", "frontend");
      return parser.parse();
    }();
    if (parsed.is_err()) {
      return false;
    }
    file.items = std::move(parsed).unwrap();
    const bool desugared = [&] {
      PROFILE_SCOPE_WITH_CATEGORY("desugar", "frontend");
      return parser::desugar_shadowing(file.items, ast, bag).is_ok();
    }();
    return desugared;
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
          diag::Severity::Error, ANALYZER_INVALID_PATH, diag::Span{});
      (void)index;
      return;
    }
    const u32 root_module =
        add_module("", root, file_data[root_file].items, NO_MODULE);
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
      modules[m]->children = ast::copy_to_arena(ast.spans, children);
    }

    for (const FileData& file : file_data) {
      if (file.module == NO_MODULE) {
        const u32 index = bag.emit<i18n::Key::AnalyzerSourceFileHasNoModule>(
            diag::Severity::Warning, ANALYZER_UNREACHABLE_FILE,
            file.path.as_view());
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
      if (segment.ends_with(".al")) {
        segment.remove_suffix(3);
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
        module_children[parent].push_back(child);
      } else if (leaf) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateModule>(
            diag::Severity::Error, ANALYZER_DUPLICATE_MODULE, diag::Span{},
            slash_name);
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
          diag::Severity::Error, ANALYZER_UNRESOLVED_IMPORT,
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
          diag::Severity::Error, ANALYZER_AMBIGUOUS_IMPORT, use_node.span,
          name);
      (void)index;
      return;
    }
    for (const Import& prior : module_imports[module]) {
      if (prior.ns == ns && prior.name == name) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateImport>(
            diag::Severity::Error, ANALYZER_AMBIGUOUS_IMPORT, use_node.span,
            name);
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
          diag::Severity::Error, ANALYZER_UNRESOLVED_IMPORT, node.span);
      (void)index;
      return;
    }
    u32 current = NO_MODULE;
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
      if (parents[module] == NO_MODULE) {
        const u32 index = bag.emit<i18n::Key::AnalyzerRootModuleHasNoParent>(
            diag::Severity::Error, ANALYZER_UNRESOLVED_IMPORT, node.span);
        (void)index;
        return;
      }
      current = parents[module];
    } else {
      current = find_child_module(module, head);
      if (current == NO_MODULE) {
        emit_unresolved(bag, ANALYZER_UNRESOLVED_IMPORT, node.span, std_hints_,
                        "import", head);
        return;
      }
    }
    for (usize i = 1; i + 1 < segments.size(); ++i) {
      current = find_child_module(current, segments[i]);
      if (current == NO_MODULE) {
        emit_unresolved(bag, ANALYZER_UNRESOLVED_IMPORT, node.span, std_hints_,
                        "import", segments[i]);
        return;
      }
    }
    const std::string_view member = segments.back();
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
      emit_unresolved(bag, ANALYZER_UNRESOLVED_IMPORT, node.span, std_hints_,
                      "import", member);
    }
  }

  std::vector<std::string> module_inputs;

  ModuleTree run(source::FileId root_id,
                 std::span<const ModuleInput> inputs,
                 std::string_view package_name_in,
                 std::span<const ModuleInput> prelude = {},
                 std::span<const StdHint> std_hints = {}) {
    package_name = package_name_in;
    root = root_id;
    std_hints_ = std_hints;
    file_data.reserve(inputs.size());
    for (const ModuleInput& input : inputs) {
      const std::optional<std::string_view> source_name =
          sources.name(input.id);
      if (!source_name.has_value()) {
        const u32 index = bag.emit<i18n::Key::AnalyzerUnknownModuleFileId>(
            diag::Severity::Error, ANALYZER_INVALID_PATH);
        (void)index;
        continue;
      }
      base::Result<path::Path, path::PathError> canonical =
          path::Path::from_native(*source_name);
      if (canonical.is_err()) {
        const u32 index = bag.emit<i18n::Key::PipelineInvalidSourcePath>(
            diag::Severity::Error, ANALYZER_INVALID_PATH);
        (void)index;
        continue;
      }
      path::Path path = std::move(canonical).unwrap();
      module_inputs.emplace_back(input.name);
      file_data.emplace_back(input.id, std::move(path));
    }
    for (const ModuleInput& input : prelude) {
      const std::optional<std::string_view> source_name =
          sources.name(input.id);
      if (!source_name.has_value()) {
        const u32 index = bag.emit<i18n::Key::AnalyzerUnknownPreludeFileId>(
            diag::Severity::Error, ANALYZER_INVALID_PATH);
        (void)index;
        continue;
      }
      base::Result<path::Path, path::PathError> canonical =
          path::Path::from_native(*source_name);
      if (canonical.is_err()) {
        const u32 index = bag.emit<i18n::Key::PipelineInvalidSourcePath>(
            diag::Severity::Error, ANALYZER_INVALID_PATH);
        (void)index;
        continue;
      }
      path::Path path = std::move(canonical).unwrap();
      prelude_names.emplace_back(input.name);
      prelude_facades.push_back(input.is_facade);
      prelude_data.emplace_back(input.id, std::move(path));
    }
    // A file that does not fit leaves the arena as it was, so every file
    // after it would report the same thing; the one diagnostic that names
    // the input is the one worth printing.
    const auto parse_all = [this](std::vector<FileData>& files) {
      for (FileData& file : files) {
        if (!lex_parse_file(file)) {
          return false;
        }
      }
      return true;
    };
    if (!parse_all(file_data) || !parse_all(prelude_data)) {
      return ModuleTree{};
    }
    build_tree();
    // A prelude package is a tree: slash-separated names nest, so
    // `core/prelude.al` and `core/mem.al` share a fileless `core` root
    // and can reach each other. Only a facade is a prelude, so only its
    // public surface is in scope without a `use`.
    const u32 modules_before = static_cast<u32>(modules.size());
    for (usize i = 0; i < prelude_data.size(); ++i) {
      const std::string& slash_name = prelude_names[i];
      const std::vector<std::string_view> segments = split_segments(slash_name);
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
              child_path, leaf ? prelude_data[i].id : source::UNKNOWN_FILE,
              leaf ? prelude_data[i].items : std::span<const ast::ItemIdx>{},
              parent);
          // The whole staged tree is toolchain sources, facades and
          // siblings alike; only facades are preludes.
          modules[child]->is_staged = true;
        } else if (leaf) {
          // Two staged sources resolved to the same leaf: the later one
          // would silently overwrite the earlier module's items.
          const u32 index = bag.emit<i18n::Key::AnalyzerDuplicatePreludeModule>(
              diag::Severity::Error, ANALYZER_DUPLICATE_MODULE,
              prelude_data[i].id == source::UNKNOWN_FILE
                  ? diag::Span{}
                  : diag::Span{prelude_data[i].id, 0, 0},
              child_path);
          (void)index;
          continue;
        }
        // A top-level module has no parent to hang off, so the root
        // case skips the edge entirely.
        if (parent != NO_MODULE) {
          module_children[parent].push_back(child);
        }
        prefix = child_path;
        parent = child;
      }
      prelude_data[i].module = parent;
      if (prelude_facades[i]) {
        modules[parent]->is_prelude = true;
        prelude_modules.push_back(parent);
      }
    }
    collect_locals();
    inject_prelude();
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      resolve_exports(m);
    }
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      modules[m]->imports = ast::copy_to_arena(ast.spans, module_imports[m]);
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
    tree.root = root_index;
    tree.prelude_modules = static_cast<u32>(prelude_modules.size());
    tree.staged_modules = static_cast<u32>(modules.size()) - modules_before;
    return tree;
  }
};

}  // namespace

base::Result<ModuleTree, diag::Reported> resolve_modules(
    source::FileId root,
    std::span<const ModuleInput> modules,
    std::string_view package_name,
    source::SourceManager& sources,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    std::span<const ModuleInput> prelude,
    std::span<const StdHint> std_hints) {
  Resolver resolver{sources, ast, bag};
  ModuleTree tree =
      resolver.run(root, modules, package_name, prelude, std_hints);
  if (bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(tree);
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
  }
  return "invalid module tree";
}

}  // namespace analyzer
