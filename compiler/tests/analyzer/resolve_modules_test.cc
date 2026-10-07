// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <deque>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "i18n/language.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace analyzer {

namespace {

struct Fixture {
  // The resolution runs through the pipeline's parse, which is what the
  // items come from, so the context owns the arena and the bag the case
  // inspects.
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  ast::AstArena& ast = ctx.ast;
  diag::DiagBag& bag = ctx.bag;
  source::SourceManager& sources = ctx.sources;
};

// The sources one case declared, held in memory. It stands in for a
// scratch directory and keeps the `dir` name so the cases below read the
// way they were written.
using VirtualDir = tests::DeclaredSources;

// Records the sources rather than writing them, so a case cannot fail for
// a reason other than what it asserts. Always succeeds, which keeps the
// call sites' guard meaningful to read.
bool write_all(
    VirtualDir& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        files) {
  for (const auto& [name, bytes] : files) {
    dir.add(name, bytes);
  }
  return true;
}

// Loads rels in order; root_rel names the entry, which rels also lists.
struct ResolveCase {
  ModuleTree tree;
  bool ok;
};

ResolveCase resolve_case(VirtualDir& dir,
                         std::string_view root_rel,
                         std::initializer_list<std::string_view> rels,
                         Fixture& f,
                         std::string_view package_name = "testpkg") {
  std::vector<ModuleInput> inputs;
  source::FileId root = source::UNKNOWN_FILE;
  std::deque<std::string> name_storage;
  for (std::string_view rel : rels) {
    std::optional<analyzer::ModuleInput> input = tests::register_source(
        f.sources, dir, rel, rel == root_rel, name_storage);
    if (!input.has_value()) {
      continue;
    }
    if (rel == root_rel) {
      root = input->id;
    }
    inputs.push_back(*input);
  }
  base::Result<ModuleTree, diag::Reported> result =
      pipeline::resolve_inputs(f.ctx, root, inputs, package_name);
  if (result.is_err()) {
    ModuleTree empty;
    empty.modules = {};
    empty.root = 0;
    return {empty, false};
  }
  ModuleTree tree = std::move(result).unwrap();
  return {tree, !f.bag.has_errors()};
}

const ModuleNode* find_module(const ModuleTree& tree, std::string_view path) {
  for (ModuleNode* const node : tree.modules) {
    if (node->path == path) {
      return node;
    }
  }
  return nullptr;
}

// What a reader can see of a resolve: the modules it produced and the
// diagnostics it reported, in the order it reported them. The node indices
// are deliberately absent, because a threaded run hands them out in whatever
// order the threads read the files in.
struct ResolveOutcome {
  std::vector<std::string> module_paths;
  std::vector<std::string> diagnostics;
};

ResolveOutcome resolve_with(VirtualDir& dir, u32 jobs) {
  Fixture f;
  f.ctx.jobs = jobs;
  ResolveOutcome outcome;
  std::deque<std::string> name_storage;
  std::vector<ModuleInput> inputs;
  source::FileId root = source::UNKNOWN_FILE;
  for (u32 i = 0; i < dir.size(); ++i) {
    const tests::VirtualSource& file = dir.at(i);
    const bool is_root = file.name == "main.al";
    std::optional<analyzer::ModuleInput> input = tests::register_source(
        f.sources, dir, file.name, is_root, name_storage);
    if (!input.has_value()) {
      continue;
    }
    if (is_root) {
      root = input->id;
    }
    inputs.push_back(*input);
  }
  base::Result<ModuleTree, diag::Reported> result =
      pipeline::resolve_inputs(f.ctx, root, inputs, "testpkg");
  if (result.is_ok()) {
    for (const ModuleNode* const node : std::move(result).unwrap().modules) {
      outcome.module_paths.emplace_back(node->path);
    }
  }
  f.bag.for_each([&outcome](const diag::Diagnostic& d) {
    // The code as a reader sees it - a letter and an id - rather than as the
    // pair it is, so the summary reads the way the report does.
    std::string code = "-";
    if (d.code.has_value()) {
      code = std::string(1, diag::stage_letter(d.code->stage)) +
             std::to_string(d.code->id);
    }
    outcome.diagnostics.push_back(
        std::to_string(static_cast<u32>(d.severity)) + "/" + code + "/" +
        std::string(d.message) + "@" +
        std::to_string(static_cast<u32>(d.primary_span.file)) + ":" +
        std::to_string(d.primary_span.offset));
  });
  return outcome;
}

}  // namespace

TEST_CASE("Resolve builds nested module trees") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"a.al", "struct Point { x: i32 }\n"},
                                        {"a/b.al", "fn deep() {}\n"},
                                        {"util.al", "fn help() {}\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "a.al", "a/b.al", "util.al"}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  CHECK(result.tree.modules.size() == 4);
  const ModuleNode* root = find_module(result.tree, "");
  const ModuleNode* a = find_module(result.tree, "a");
  const ModuleNode* util = find_module(result.tree, "util");
  const ModuleNode* b = find_module(result.tree, "a::b");
  CHECK(root != nullptr);
  CHECK(a != nullptr);
  CHECK(util != nullptr);
  CHECK(b != nullptr);
  if (a == nullptr || b == nullptr) {
    return;
  }
  CHECK(a->items.size() == 1);
  CHECK(b->items.size() == 1);
  CHECK(result.tree.modules[result.tree.root]->path.empty());
}

TEST_CASE("Resolve attaches deeply nested modules") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"x/y/z.al", "fn deep() {}\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "x/y/z.al"}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  CHECK(find_module(result.tree, "x::y::z") != nullptr);
}

TEST_CASE("Resolve reports duplicate module declarations") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"a.al", "fn x() {}\n"},
                                        {"sub/a.al", "fn y() {}\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  // Two files claiming one slash name collide regardless of paths.
  // Registered by hand because two of them claim the module name "a", and
  // going through the rel list would register only the first.
  const tests::VirtualSource* const a = dir.find("a.al");
  const tests::VirtualSource* const nested_a = dir.find("sub/a.al");
  const tests::VirtualSource* const main = dir.find("main.al");
  CHECK(a != nullptr);
  CHECK(nested_a != nullptr);
  CHECK(main != nullptr);
  if (a == nullptr || nested_a == nullptr || main == nullptr) {
    return;
  }
  const ModuleInput inputs[] = {
      {"", f.sources.add_virtual(main->name, main->bytes)},
      {"a", f.sources.add_virtual(a->name, a->bytes)},
      {"a", f.sources.add_virtual(nested_a->name, nested_a->bytes)},
  };
  base::Result<ModuleTree, diag::Reported> resolved =
      pipeline::resolve_inputs(f.ctx, inputs[0].id, inputs, "testpkg");
  CHECK(f.bag.has_errors());
}

TEST_CASE("Resolve attaches unreferenced files as modules") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"stray.al", "fn stray() {}\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  // Every listed input attaches, so a stray module is not an error.
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "stray.al"}, f);
  CHECK(result.ok);
}

TEST_CASE("Resolve resolves imports across modules") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {
               {"main.al",
                "use a::Point;\nuse util::help as h;\nfn "
                "main() {}\n"},
               {"a.al", "pub struct Point { x: i32 }\nuse package::util;\n"},
               {"util.al", "fn help() {}\n"},
           });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "a.al", "util.al"}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ModuleNode* root = find_module(result.tree, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  // Point contributes type and value entries; h one value entry.
  CHECK(root->imports.size() == 3);
  if (root->imports.size() != 3) {
    return;
  }
  const ModuleNode* a = find_module(result.tree, "a");
  const ModuleNode* util = find_module(result.tree, "util");
  CHECK(a != nullptr);
  CHECK(util != nullptr);
  if (a == nullptr || util == nullptr) {
    return;
  }
  u32 a_index = 0;
  u32 util_index = 0;
  u32 root_index = 0;
  for (u32 i = 0; i < static_cast<u32>(result.tree.modules.size()); ++i) {
    if (result.tree.modules[i] == a) {
      a_index = i;
    }
    if (result.tree.modules[i] == util) {
      util_index = i;
    }
    if (result.tree.modules[i] == root) {
      root_index = i;
    }
  }
  CHECK(root->imports[0].name == "Point");
  CHECK(root->imports[0].ns == Namespace::Type);
  CHECK(root->imports[0].target_module == a_index);
  CHECK(root->imports[1].name == "Point");
  CHECK(root->imports[1].ns == Namespace::Value);
  CHECK(root->imports[2].name == "h");
  CHECK(root->imports[2].target_module == util_index);
  CHECK(root->imports[2].member == "help");
  CHECK(a->imports.size() == 1);
  if (a->imports.size() == 1) {
    CHECK(a->imports[0].name == "util");
    CHECK(a->imports[0].ns == Namespace::Module);
    CHECK(a->imports[0].target_module == root_index);
    CHECK(a->imports[0].member == "util");
  }
}

TEST_CASE("Resolve handles super imports from nested modules") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {
                         {"main.al", "fn main() {}\n"},
                         {"a.al", "struct Thing { x: i32 }\n"},
                         {"a/b.al", "use super::Thing;\nfn deep() {}\n"},
                     });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "a.al", "a/b.al"}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ModuleNode* b = find_module(result.tree, "a::b");
  CHECK(b != nullptr);
  if (b == nullptr) {
    return;
  }
  CHECK(b->imports.size() == 2);
}

TEST_CASE("Resolve reports bad imports") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"a.al", "struct Point { x: i32 }\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f2;
  {
    VirtualDir dir2;
    const bool setup2 =
        write_all(dir2, {{"main.al", "use a::Nope;\nfn main() {}\n"},
                         {"a.al", "struct Point { x: i32 }\n"}});
    CHECK(setup2);
    if (!setup2) {
      return;
    }
    const ResolveCase result =
        resolve_case(dir2, "main.al", {"main.al", "a.al"}, f2);
    CHECK(!result.ok);
    CHECK(f2.bag.has_errors());
  }

  Fixture f3;
  {
    VirtualDir dir3;
    const bool setup3 =
        write_all(dir3, {{"main.al", "use foo;\nfn main() {}\n"}});
    CHECK(setup3);
    if (!setup3) {
      return;
    }
    const ResolveCase result = resolve_case(dir3, "main.al", {"main.al"}, f3);
    CHECK(!result.ok);
    CHECK(f3.bag.has_errors());
  }

  Fixture f4;
  {
    VirtualDir dir4;
    const bool setup4 =
        write_all(dir4, {{"main.al", "use super::x;\nfn main() {}\n"}});
    CHECK(setup4);
    if (!setup4) {
      return;
    }
    const ResolveCase result = resolve_case(dir4, "main.al", {"main.al"}, f4);
    CHECK(!result.ok);
    CHECK(f4.bag.has_errors());
  }
}

TEST_CASE("Resolve reports conflicting imports") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {
               {"main.al", "use a::Thing;\nuse b::Thing;\nfn main() {}\n"},
               {"a.al", "struct Thing { x: i32 }\n"},
               {"b.al", "struct Thing { x: i32 }\n"},
           });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "a.al", "b.al"}, f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Resolve follows public re-exports") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {
                         {"main.al", "use a::Thing;\nfn main() {}\n"},
                         {"a.al", "pub use b::Thing;\n"},
                         {"a/b.al", "struct Thing { x: i32 }\n"},
                     });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "a.al", "a/b.al"}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ModuleNode* root = find_module(result.tree, "");
  const ModuleNode* b = find_module(result.tree, "a::b");
  CHECK(root != nullptr);
  CHECK(b != nullptr);
  if (root == nullptr || b == nullptr) {
    return;
  }
  u32 b_index = 0;
  for (u32 i = 0; i < static_cast<u32>(result.tree.modules.size()); ++i) {
    if (result.tree.modules[i] == b) {
      b_index = i;
    }
  }
  bool found_type = false;
  bool found_value = false;
  for (const Import& import : root->imports) {
    if (import.name == "Thing" && import.target_module == b_index) {
      if (import.ns == Namespace::Type) {
        found_type = true;
      }
      if (import.ns == Namespace::Value) {
        found_value = true;
      }
    }
  }
  CHECK(found_type);
  CHECK(found_value);
}

TEST_CASE("Resolve reports re-export cycles") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"a.al", "pub use b::Thing;\n"},
                                        {"b.al", "pub use a::Thing;\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "a.al", "b.al"}, f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

ResolveCase resolve_case_with_prelude(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude,
    Fixture& f) {
  std::deque<std::string> name_storage;
  std::vector<ModuleInput> inputs;
  source::FileId root = source::UNKNOWN_FILE;
  for (std::string_view rel : rels) {
    std::optional<ModuleInput> input = tests::register_source(
        f.sources, dir, rel, rel == root_rel, name_storage);
    if (!input.has_value()) {
      continue;
    }
    if (rel == root_rel) {
      root = input->id;
    }
    inputs.push_back(*input);
  }
  std::deque<std::string> prelude_storage;
  std::vector<ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    prelude_storage.emplace_back(name);
    // A staged prelude source is a package facade, so its public
    // surface is in scope without a `use`; see
    // docs/adr/0016-suites-and-the-std-split.md.
    prelude_inputs.push_back({prelude_storage.back(),
                              f.sources.add_virtual(file->name, file->bytes),
                              true});
  }
  base::Result<ModuleTree, diag::Reported> result =
      pipeline::resolve_inputs(f.ctx, root, inputs, "testpkg", prelude_inputs);
  if (result.is_err()) {
    ModuleTree empty;
    empty.modules = {};
    empty.root = 0;
    return {empty, false};
  }
  ModuleTree tree = std::move(result).unwrap();
  return {tree, !f.bag.has_errors()};
}

// Resolves a package and one path dependency the way the
// pipeline stages them: every source parses once, the
// package's own modules root at `root_rel`, and the
// dependency's modules sit behind a fileless root named by
// `identity`, trimmed to `exports`. `dependency_modules` and
// `export_storage` are the storage the tree's package root
// borrows, so a caller keeps both as long as it reads the
// tree.
base::Result<ModuleTree, diag::Reported> resolve_dep_case(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    VirtualDir& dep_dir,
    std::string_view identity,
    std::initializer_list<std::string_view> exports,
    std::initializer_list<std::string_view> dep_rels,
    Fixture& f,
    std::vector<ParsedModule>& dependency_modules,
    std::vector<std::string_view>& export_storage,
    std::string_view package_name = "testpkg") {
  std::deque<std::string> name_storage;
  std::vector<ModuleInput> inputs;
  std::vector<ModuleInput> dep_inputs;
  source::FileId root = source::UNKNOWN_FILE;
  for (std::string_view rel : rels) {
    std::optional<ModuleInput> input = tests::register_source(
        f.sources, dir, rel, rel == root_rel, name_storage);
    if (!input.has_value()) {
      continue;
    }
    if (rel == root_rel) {
      root = input->id;
    }
    inputs.push_back(*input);
  }
  std::vector<source::FileId> parse_ids;
  parse_ids.reserve(rels.size() + dep_rels.size());
  for (const ModuleInput& input : inputs) {
    parse_ids.push_back(input.id);
  }
  for (std::string_view rel : dep_rels) {
    std::optional<ModuleInput> input =
        tests::register_source(f.sources, dep_dir, rel, false, name_storage);
    if (!input.has_value()) {
      continue;
    }
    parse_ids.push_back(input->id);
    dep_inputs.push_back(*input);
  }
  base::Result<pipeline::ParsedFiles, diag::Reported> parsed =
      pipeline::parse_files(f.ctx, parse_ids);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pipeline::ParsedFiles files = std::move(parsed).unwrap();
  std::vector<ParsedModule> modules;
  modules.reserve(inputs.size());
  for (const ModuleInput& input : inputs) {
    modules.push_back(pipeline::parsed_module(files, input));
  }
  dependency_modules.reserve(dep_inputs.size());
  for (const ModuleInput& input : dep_inputs) {
    dependency_modules.push_back(pipeline::parsed_module(files, input));
  }
  export_storage.reserve(exports.size());
  for (std::string_view exported : exports) {
    export_storage.push_back(exported);
  }
  const DependencyPackage dependency{
      identity, std::span<const std::string_view>(export_storage),
      std::span<const ParsedModule>(dependency_modules)};
  return resolve_modules(root, modules, package_name, f.ctx.ast, f.bag, {}, {},
                         std::span<const DependencyPackage>{&dependency, 1});
}

TEST_CASE("Resolve nests a facade beside its sibling modules") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {
                         {"main.al", "fn main() {}\n"},
                         {"core/prelude.al", "pub use super::mem::help;\n"},
                         {"core/mem.al", "pub fn help() {}\n"},
                     });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  std::deque<std::string> name_storage;
  std::vector<ModuleInput> inputs;
  std::optional<ModuleInput> root_input =
      tests::register_source(f.sources, dir, "main.al", true, name_storage);
  CHECK(root_input.has_value());
  if (!root_input.has_value()) {
    return;
  }
  inputs.push_back(*root_input);
  std::deque<std::string> prelude_storage;
  std::vector<ModuleInput> prelude_inputs;
  for (const auto& entry :
       {std::tuple<std::string_view, std::string_view, bool>{
            "core/prelude.al", "core/prelude.al", true},
        std::tuple<std::string_view, std::string_view, bool>{
            "core/mem.al", "core/mem.al", false}}) {
    const std::string_view name = std::get<0>(entry);
    const std::string_view rel = std::get<1>(entry);
    const bool facade = std::get<2>(entry);
    const tests::VirtualSource* const file = dir.find(rel);
    CHECK(file != nullptr);
    if (file == nullptr) {
      return;
    }
    prelude_storage.emplace_back(name);
    prelude_inputs.push_back({prelude_storage.back(),
                              f.sources.add_virtual(file->name, file->bytes),
                              facade});
  }
  base::Result<ModuleTree, diag::Reported> resolved = pipeline::resolve_inputs(
      f.ctx, root_input->id, inputs, "testpkg", prelude_inputs);
  CHECK(resolved.is_ok());
  if (resolved.is_err()) {
    return;
  }
  ModuleTree tree = std::move(resolved).unwrap();
  CHECK(!f.bag.has_errors());
  const ModuleNode* root = find_module(tree, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  bool found = false;
  for (const Import& import : root->imports) {
    if (import.ns == Namespace::Value && import.name == "help" &&
        import.member == "help") {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("Resolve injects prelude imports") {
  VirtualDir dir;
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"core.al", "pub fn help() {}\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result = resolve_case_with_prelude(
      dir, "main.al", {"main.al"}, {{"core", "core.al"}}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ModuleNode* root = find_module(result.tree, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  bool found = false;
  for (const Import& import : root->imports) {
    if (import.ns == Namespace::Value && import.name == "help" &&
        import.member == "help") {
      found = true;
    }
  }
  CHECK(found);
  const ModuleNode* core = find_module(result.tree, "core");
  CHECK(core != nullptr);
}

TEST_CASE("Resolve prefers locals over prelude imports") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {
                         {"main.al", "fn help() {}\nfn main() {}\n"},
                         {"core.al", "pub fn help() {}\n"},
                     });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const ResolveCase result = resolve_case_with_prelude(
      dir, "main.al", {"main.al"}, {{"core", "core.al"}}, f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ModuleNode* root = find_module(result.tree, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  for (const Import& import : root->imports) {
    CHECK(!(import.ns == Namespace::Value && import.name == "help"));
  }
}

TEST_CASE("Resolve stages a dependency behind its fileless root") {
  VirtualDir dir;
  VirtualDir dep_dir;
  const bool setup = write_all(
      dir, {{"main.al", "use acme_hash::sha2::digest;\nfn main() {}\n"}});
  CHECK(setup);
  const bool dep_setup = write_all(dep_dir, {
                                                {"sha2.al",
                                                 "pub fn digest(x: i32) -> "
                                                 "i32 {\n  ret x + 1\n}\n"},
                                                {"detail.al",
                                                 "pub fn helper() -> i32 {\n "
                                                 " ret 0\n}\n"},
                                            });
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  Fixture f;
  std::vector<ParsedModule> dependency_modules;
  std::vector<std::string_view> export_storage;
  base::Result<ModuleTree, diag::Reported> result = resolve_dep_case(
      dir, "main.al", {"main.al"}, dep_dir, "acme_hash", {"sha2"},
      {"sha2.al", "detail.al"}, f, dependency_modules, export_storage);
  CHECK(result.is_ok());
  CHECK(!f.bag.has_errors());
  if (result.is_err() || f.bag.has_errors()) {
    return;
  }
  ModuleTree tree = std::move(result).unwrap();
  CHECK(tree.package_roots.size() == 1);
  if (tree.package_roots.size() != 1) {
    return;
  }
  CHECK(tree.package_roots[0].identity == "acme_hash");
  CHECK(tree.package_roots[0].trimmed);
  const ModuleNode* sha2 = find_module(tree, "acme_hash::sha2");
  const ModuleNode* detail = find_module(tree, "acme_hash::detail");
  const ModuleNode* root = find_module(tree, "");
  CHECK(sha2 != nullptr);
  CHECK(detail != nullptr);
  CHECK(root != nullptr);
  if (sha2 == nullptr || detail == nullptr || root == nullptr) {
    return;
  }
  u32 sha2_index = 0;
  for (u32 i = 0; i < static_cast<u32>(tree.modules.size()); ++i) {
    if (tree.modules[i] == sha2) {
      sha2_index = i;
    }
    if (tree.modules[i] == root) {
      CHECK(tree.module_root(i) == NO_PACKAGE_ROOT);
    } else {
      CHECK(tree.module_root(i) == 0);
    }
  }
  // The staged modules are ordinary: the resolver still reports the
  // toolchain's staged sources apart, so nothing here is staged.
  CHECK(tree.staged_modules == 0);
  bool found = false;
  for (const Import& import : root->imports) {
    if (import.ns == Namespace::Value && import.name == "digest" &&
        import.target_module == sha2_index && import.member == "digest") {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("Resolve trims a dependency to its export list") {
  VirtualDir dir;
  VirtualDir dep_dir;
  const bool setup = write_all(
      dir, {{"main.al", "use acme_hash::detail::helper;\nfn main() {}\n"}});
  CHECK(setup);
  const bool dep_setup = write_all(dep_dir, {
                                                {"sha2.al",
                                                 "pub fn digest(x: i32) -> "
                                                 "i32 {\n  ret x + 1\n}\n"},
                                                {"detail.al",
                                                 "pub fn helper() -> i32 {\n "
                                                 " ret 0\n}\n"},
                                            });
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  Fixture f;
  std::vector<ParsedModule> dependency_modules;
  std::vector<std::string_view> export_storage;
  base::Result<ModuleTree, diag::Reported> result = resolve_dep_case(
      dir, "main.al", {"main.al"}, dep_dir, "acme_hash", {"sha2"},
      {"sha2.al", "detail.al"}, f, dependency_modules, export_storage);
  CHECK(result.is_err());
  CHECK(f.bag.has_errors());
  bool named = false;
  f.bag.for_each([&](const diag::Diagnostic& diagnostic) {
    named = named || diagnostic.message ==
                         "Package 'acme_hash' does not export module "
                         "'detail'";
  });
  CHECK(named);
}

TEST_CASE("Resolve keeps a dependency's own uses untrimmed") {
  VirtualDir dir;
  VirtualDir dep_dir;
  const bool setup =
      write_all(dir, {{"main.al", "use acme_hash::api::go;\nfn main() {}\n"}});
  CHECK(setup);
  const bool dep_setup = write_all(
      dep_dir, {{"api.al",
                 "use acme_hash::detail::helper;\npub fn go() -> i32 {\n  ret "
                 "helper()\n}\n"},
                {"detail.al", "pub fn helper() -> i32 {\n  ret 0\n}\n"}});
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  Fixture f;
  std::vector<ParsedModule> dependency_modules;
  std::vector<std::string_view> export_storage;
  base::Result<ModuleTree, diag::Reported> result = resolve_dep_case(
      dir, "main.al", {"main.al"}, dep_dir, "acme_hash", {"api"},
      {"api.al", "detail.al"}, f, dependency_modules, export_storage);
  CHECK(result.is_ok());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Resolve prefers a package root over a same-named module") {
  VirtualDir dir;
  VirtualDir dep_dir;
  const bool setup =
      write_all(dir, {{"main.al", "use hash::sha2::digest;\nfn main() {}\n"},
                      {"hash.al", "pub fn local() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  const bool dep_setup = write_all(
      dep_dir,
      {{"sha2.al", "pub fn digest(x: i32) -> i32 {\n  ret x + 1\n}\n"}});
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  Fixture f;
  std::vector<ParsedModule> dependency_modules;
  std::vector<std::string_view> export_storage;
  base::Result<ModuleTree, diag::Reported> result = resolve_dep_case(
      dir, "main.al", {"main.al", "hash.al"}, dep_dir, "hash", {"sha2"},
      {"sha2.al"}, f, dependency_modules, export_storage);
  CHECK(result.is_ok());
  CHECK(!f.bag.has_errors());
  if (result.is_err() || f.bag.has_errors()) {
    return;
  }
  ModuleTree tree = std::move(result).unwrap();
  // Both the package's own module and the dependency's root read
  // "hash": the import still reaches the dependency, while the
  // package's module answers to `package::` or `self::`.
  const ModuleNode* sha2 = find_module(tree, "hash::sha2");
  const ModuleNode* root = find_module(tree, "");
  CHECK(sha2 != nullptr);
  CHECK(root != nullptr);
  if (sha2 == nullptr || root == nullptr) {
    return;
  }
  u32 sha2_index = 0;
  for (u32 i = 0; i < static_cast<u32>(tree.modules.size()); ++i) {
    if (tree.modules[i] == sha2) {
      sha2_index = i;
    }
  }
  bool found = false;
  for (const Import& import : root->imports) {
    if (import.ns == Namespace::Value && import.name == "digest" &&
        import.target_module == sha2_index) {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("Module tree verification rejects malformed trees") {
  const ModuleTree empty{.modules = {}, .root = 0};
  CHECK(verify_module_tree(empty).is_err());

  ModuleNode node;
  node.path = "";
  node.file = source::UNKNOWN_FILE;
  ModuleNode* const one[] = {&node};
  const ModuleTree bad_root{.modules = {one, 1}, .root = 5};
  CHECK(verify_module_tree(bad_root).is_err());

  ModuleNode* const null_entry[] = {nullptr};
  const ModuleTree null_module{.modules = {null_entry, 1}, .root = 0};
  CHECK(verify_module_tree(null_module).is_err());

  const ModuleTree bad_prelude{
      .modules = {one, 1}, .root = 0, .prelude_modules = 2};
  CHECK(verify_module_tree(bad_prelude).is_err());

  const u32 one_root[] = {NO_PACKAGE_ROOT, NO_PACKAGE_ROOT};
  const ModuleTree bad_module_roots{
      .modules = {one, 1}, .root = 0, .module_roots = {one_root, 2}};
  CHECK(verify_module_tree(bad_module_roots).is_err());

  const u32 covered[] = {NO_PACKAGE_ROOT};
  const ModuleTree valid{
      .modules = {one, 1}, .root = 0, .module_roots = {covered, 1}};
  CHECK(verify_module_tree(valid).is_ok());
}

TEST_CASE("Check package rejects a malformed module tree") {
  Fixture f;
  ir::SymbolTable strings{mem::page_size()};
  const ModuleTree empty{.modules = {}, .root = 0};
  CHECK(check_package(empty, ir::PointerWidth::W64, f.ast, f.bag, strings)
            .is_err());
  CHECK(f.bag.has_errors());
}

// Reading files on several threads gives the same answer as reading them on
// one: the same modules, and the same diagnostics in the same order. The node
// indices are not compared, because a threaded run hands them out in whatever
// order the threads read the files in - what has to hold is that a reader
// cannot tell the two apart.
//
// The input is a dozen files with a diagnostic in each, so that the threads
// have work to disagree about and the bags have something to order.
TEST_CASE("Resolving on several threads gives what resolving on one gives") {
  VirtualDir dir;
  // The declarations are views, so the names and the text have to outlive
  // the directory that points at them.
  std::deque<std::string> names;
  std::deque<std::string> text;
  dir.add("main.al", "fn main() {}\n");
  for (u32 i = 0; i < 12; ++i) {
    names.push_back("m" + std::to_string(i) + ".al");
    // A name the module does not declare is an unresolved import, which is
    // a diagnostic with a span and an order.
    text.push_back("import \"nowhere::missing" + std::to_string(i) + "\"\n");
    dir.add(names.back(), text.back());
  }

  const ResolveOutcome serial = resolve_with(dir, 1);
  const ResolveOutcome threaded = resolve_with(dir, 8);

  CHECK(serial.module_paths == threaded.module_paths);
  CHECK(serial.diagnostics.size() == 12);
  CHECK(serial.diagnostics == threaded.diagnostics);
}

}  // namespace analyzer
