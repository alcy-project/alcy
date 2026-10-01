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
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_interner.h"
#include "i18n/language.h"
#include "ir/type.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace analyzer {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  source::SourceManager sources;

  Fixture() { arena.reserve(1u << 20); }
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
      resolve_modules(root, inputs, package_name, f.sources, f.ast, f.bag);
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
      resolve_modules(inputs[0].id, inputs, "testpkg", f.sources, f.ast, f.bag);
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

// A run whose arena is spent refuses the next file with a diagnostic,
// where the arena's own report is a trap naming neither. The reservation
// here is small enough for a case to spend it, which a real input would
// need far too much source to reach.
TEST_CASE("Resolve refuses an input the span arena cannot hold") {
  constexpr usize CAPACITY = 4096;
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al", "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  mem::Arena arena;
  arena.reserve(1u << 20);
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  source::SourceManager sources;
  ast::AstArena ast{CAPACITY};
  // Past the headroom, which is what the check asks about.
  CHECK(ast.spans.alloc(CAPACITY - 1) != nullptr);
  CHECK(ast.spans_nearly_full());

  std::deque<std::string> name_storage;
  std::optional<analyzer::ModuleInput> input =
      tests::register_source(sources, dir, "main.al", true, name_storage);
  CHECK(input.has_value());
  if (!input.has_value()) {
    return;
  }
  const std::vector<ModuleInput> inputs{*input};
  base::Result<ModuleTree, diag::Reported> result =
      resolve_modules(input->id, inputs, "testpkg", sources, ast, bag);
  CHECK(result.is_err());
  CHECK(bag.has_errors());
}

// A reservation with room in it reads what it is given, so the check does
// not refuse an input for being large when the arena can hold it.
TEST_CASE("Resolve reads an input the span arena can hold") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al", "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!f.ast.spans_nearly_full());
  const ResolveCase result = resolve_case(dir, "main.al", {"main.al"}, f);
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
  base::Result<ModuleTree, diag::Reported> result = resolve_modules(
      root, inputs, "testpkg", f.sources, f.ast, f.bag, prelude_inputs);
  if (result.is_err()) {
    ModuleTree empty;
    empty.modules = {};
    empty.root = 0;
    return {empty, false};
  }
  ModuleTree tree = std::move(result).unwrap();
  return {tree, !f.bag.has_errors()};
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
  base::Result<ModuleTree, diag::Reported> resolved =
      resolve_modules(root_input->id, inputs, "testpkg", f.sources, f.ast,
                      f.bag, prelude_inputs);
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

  const ModuleTree valid{.modules = {one, 1}, .root = 0};
  CHECK(verify_module_tree(valid).is_ok());
}

TEST_CASE("Check package rejects a malformed module tree") {
  Fixture f;
  str::StringInterner strings{mem::page_size()};
  const ModuleTree empty{.modules = {}, .root = 0};
  CHECK(check_package(empty, ir::PointerWidth::W64, f.ast, f.bag, strings)
            .is_err());
  CHECK(f.bag.has_errors());
}

}  // namespace analyzer
