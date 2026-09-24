// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace analyzer {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena};
  source::SourceManager sources;

  Fixture() { arena.reserve(1u << 20); }
};

bool write_all(
    io::TempDir& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        files) {
  for (const auto& [rel, content] : files) {
    if (!dir.write_file(rel, content)) {
      return false;
    }
  }
  return true;
}

// Loads rels in order and resolves with the first as an explicit root
// stand-in: callers pass the root rel separately for clarity.
struct ResolveCase {
  ModuleTree tree;
  bool ok;
};

ResolveCase resolve_case(io::TempDir& dir,
                         std::string_view root_rel,
                         std::initializer_list<std::string_view> rels,
                         Fixture& f,
                         std::string_view package_name = "testpkg") {
  std::vector<ModuleInput> inputs;
  source::FileId root = source::kUnknownFile;
  for (std::string_view rel : rels) {
    base::Result<source::FileId, source::SourceError> loaded =
        f.sources.load(dir.join(rel));
    if (loaded.is_err()) {
      continue;
    }
    const source::FileId id = std::move(loaded).unwrap();
    if (rel == root_rel) {
      root = id;
      inputs.push_back({"", id});
    } else {
      std::string_view name = rel;
      constexpr std::string_view suffix = ".al";
      if (name.size() > suffix.size() &&
          name.substr(name.size() - suffix.size()) == suffix) {
        name.remove_suffix(suffix.size());
      }
      inputs.push_back({name, id});
    }
  }
  diag::Fallible<ModuleTree> result =
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
  io::TempDir dir("alcy_analyzer_tree_test");
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
  io::TempDir dir("alcy_analyzer_deep_test");
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
  io::TempDir dir("alcy_analyzer_dup_test");
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
  base::Result<source::FileId, source::SourceError> first =
      f.sources.load(dir.join("a.al"));
  base::Result<source::FileId, source::SourceError> second =
      f.sources.load(dir.join("sub/a.al"));
  base::Result<source::FileId, source::SourceError> root =
      f.sources.load(dir.join("main.al"));
  CHECK(first.is_ok());
  CHECK(second.is_ok());
  CHECK(root.is_ok());
  if (first.is_err() || second.is_err() || root.is_err()) {
    return;
  }
  const ModuleInput inputs[] = {
      {"", std::move(root).unwrap()},
      {"a", std::move(first).unwrap()},
      {"a", std::move(second).unwrap()},
  };
  diag::Fallible<ModuleTree> resolved =
      resolve_modules(inputs[0].id, inputs, "testpkg", f.sources, f.ast, f.bag);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Resolve attaches unreferenced files as modules") {
  io::TempDir dir("alcy_analyzer_unreachable_test");
  const bool setup = write_all(dir, {
                                        {"main.al", "fn main() {}\n"},
                                        {"stray.al", "fn stray() {}\n"},
                                    });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  // Every listed input attaches, so the resolve-level warning only
  // fires through duplicate collisions (covered above); the
  // manifest-aware version lands with cli warnings.
  const ResolveCase result =
      resolve_case(dir, "main.al", {"main.al", "stray.al"}, f);
  CHECK(result.ok);
}

TEST_CASE("Resolve resolves imports across modules") {
  io::TempDir dir("alcy_analyzer_use_test");
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
  io::TempDir dir("alcy_analyzer_super_test");
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
  io::TempDir dir("alcy_analyzer_unresolved_test");
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
    io::TempDir dir2("alcy_analyzer_unresolved2_test");
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
    io::TempDir dir3("alcy_analyzer_unresolved3_test");
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
    io::TempDir dir4("alcy_analyzer_unresolved4_test");
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
  io::TempDir dir("alcy_analyzer_ambiguous_test");
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
  io::TempDir dir("alcy_analyzer_reexport_test");
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
  io::TempDir dir("alcy_analyzer_cycle_test");
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
    io::TempDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude,
    Fixture& f) {
  std::vector<ModuleInput> inputs;
  source::FileId root = source::kUnknownFile;
  for (std::string_view rel : rels) {
    base::Result<source::FileId, source::SourceError> loaded =
        f.sources.load(dir.join(rel));
    if (loaded.is_err()) {
      continue;
    }
    const source::FileId id = std::move(loaded).unwrap();
    if (rel == root_rel) {
      root = id;
      inputs.push_back({"", id});
    } else {
      inputs.push_back({rel, id});
    }
  }
  std::vector<std::string> prelude_storage;
  std::vector<ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    base::Result<source::FileId, source::SourceError> loaded =
        f.sources.load(dir.join(rel));
    if (loaded.is_err()) {
      continue;
    }
    prelude_storage.emplace_back(name);
    prelude_inputs.push_back(
        {prelude_storage.back(), std::move(loaded).unwrap()});
  }
  diag::Fallible<ModuleTree> result = resolve_modules(
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

TEST_CASE("Resolve injects prelude imports") {
  io::TempDir dir("alcy_analyzer_prelude_test");
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
  io::TempDir dir("alcy_analyzer_prelude_shadow_test");
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

}  // namespace analyzer
