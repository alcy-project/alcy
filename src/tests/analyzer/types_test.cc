// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/types.h"

#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
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

struct CheckCase {
  std::optional<CheckedPackage> package;
};

CheckCase check_case(
    io::TempDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    Fixture& f,
    ir::PointerWidth width = ir::PointerWidth::W64,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude = {}) {
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
  diag::Fallible<ModuleTree> tree_result = resolve_modules(
      root, inputs, "testpkg", f.sources, f.ast, f.bag, prelude_inputs);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt};
  }
  ModuleTree tree = std::move(tree_result).unwrap();
  CheckedPackage checked = check_package(tree, width, f.ast, f.bag).unwrap();
  if (f.bag.has_errors()) {
    return {std::nullopt};
  }
  return {std::move(checked)};
}

const CheckedModule* find_checked(const CheckedPackage& package,
                                  std::string_view path) {
  for (const CheckedModule& module : package.modules) {
    if (package.tree.modules[module.module]->path == path) {
      return &module;
    }
  }
  return nullptr;
}

const CheckedModule::NamedType* find_type(const CheckedModule& module,
                                          std::string_view name) {
  for (const CheckedModule::NamedType& type : module.types) {
    if (type.name == name) {
      return &type;
    }
  }
  return nullptr;
}

// `Option` and `Result` live in the core prelude since ADR-0009, so
// tests that mention them inject a matching declaration set.
constexpr std::string_view kCorePrelude =
    R"(pub intrinsic fn panic(msg: str) -> !;
pub enum Option<T> { Some(T), None }
pub enum Result<T, E> { Ok(T), Err(E) }
)";

constexpr std::string_view kCorePreludeFile = "core.al";

// Static storage keeps the returned initializer_list valid for the
// caller's use; an initializer_list of temporaries would dangle.
const std::initializer_list<std::pair<std::string_view, std::string_view>>&
core_prelude() {
  static const std::initializer_list<
      std::pair<std::string_view, std::string_view>>
      kPrelude = {{"core", kCorePreludeFile}};
  return kPrelude;
}

}  // namespace

TEST_CASE("Check interns structs with named fields") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_struct_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Point { x: i32, y: i32 }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  const CheckedModule::NamedType* point = find_type(*root, "Point");
  CHECK(point != nullptr);
  if (point == nullptr) {
    return;
  }
  CHECK(result.package->types.types()[point->type].tag == ir::TypeTag::Struct);
  CHECK(result.package->types.is_copy_type(point->type));
  bool fields_ok = false;
  for (const CheckedModule::StructInfo& info : root->structs) {
    if (info.type.idx == point->type.idx) {
      fields_ok = info.fields.size() == 2 && info.fields[0] == "x" &&
                  info.fields[1] == "y";
    }
  }
  CHECK(fields_ok);
}

TEST_CASE("Check interns enums with payloads") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_enum_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Choice { Yes, No(i32) }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  const CheckedModule::NamedType* choice =
      root != nullptr ? find_type(*root, "Choice") : nullptr;
  CHECK(choice != nullptr);
  if (choice == nullptr) {
    return;
  }
  CHECK(result.package->types.types()[choice->type].tag == ir::TypeTag::Enum);
  CHECK(result.package->types.is_copy_type(choice->type));
}

TEST_CASE("Check instantiates generic structs") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_struct_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct Pair<A, B> { a: A, b: B }\n"
                       "fn f(x: Pair<i32, str>) -> Pair<i32, str> {\n"
                       "  ret x\n"
                       "}\n"
                       "fn g(x: Pair<str, i32>) -> i32 {\n"
                       "  ret 0\n"
                       "}\n"
                       "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.size() < 2) {
    return;
  }
  const ir::Storage& types = result.package->types;
  const ir::TypeIdx pair_i_s = root->functions[0].params[0];
  CHECK(types.types()[pair_i_s].tag == ir::TypeTag::Struct);
  // Identical instantiations share one index; different ones do not.
  CHECK(pair_i_s.idx == root->functions[0].ret.idx);
  CHECK(pair_i_s.idx != root->functions[1].params[0].idx);
}

TEST_CASE("Check instantiates generic struct methods") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_struct_method_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Pair<A, B> { a: A, b: B }\n"
                                      "impl<A, B> Pair<A, B> {\n"
                                      "  fn first(self: Self) -> A {\n"
                                      "    ret self.a\n"
                                      "  }\n"
                                      "}\n"
                                      "fn f(p: Pair<i32, bool>) -> i32 {\n"
                                      "  ret p.first()\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects arity mismatch on generic structs") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_struct_arity_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Pair<A, B> { a: A, b: B }\n"
                                      "fn f(x: Pair<i32>) -> i32 {\n"
                                      "  ret 0\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves annotations and signatures") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_sig_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: i32, b: &i32, c: (i32, bool), d: !) -> str {\n"
                       "  ret \"x\"\n"
                       "}\n"
                       "static count: u64 = 0\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.empty()) {
    return;
  }
  const CheckedModule::FnSig& sig = root->functions[0];
  CHECK(sig.name == "f");
  CHECK(sig.params.size() == 4);
  if (sig.params.size() != 4) {
    return;
  }
  const ir::Storage& types = result.package->types;
  CHECK(types.types()[sig.params[0]].tag == ir::TypeTag::I32);
  CHECK(types.types()[sig.params[1]].tag == ir::TypeTag::Ref);
  CHECK(types.types()[sig.params[2]].tag == ir::TypeTag::Tuple);
  CHECK(types.types()[sig.params[3]].tag == ir::TypeTag::Never);
  CHECK(types.types()[sig.ret].tag == ir::TypeTag::Str);
  CHECK(!root->statics.empty());
  CHECK(root->statics[0].name == "count");
  CHECK(types.types()[root->statics[0].type].tag == ir::TypeTag::U64);
}

TEST_CASE("Check instantiates generic enums") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_generic_enum_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "fn f(x: Box<i32>) -> Box<i32> {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn g(x: Box<Box<i32>>) -> i32 {\n"
                                      "  ret 0\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.empty()) {
    return;
  }
  const CheckedModule::FnSig& sig = root->functions[0];
  CHECK(sig.name == "f");
  const ir::Storage& types = result.package->types;
  CHECK(types.types()[sig.params[0]].tag == ir::TypeTag::Enum);
  CHECK(types.types()[sig.ret].tag == ir::TypeTag::Enum);
  CHECK(sig.params[0].idx == sig.ret.idx);
  if (root->functions.size() < 2) {
    return;
  }
  const CheckedModule::FnSig& nested = root->functions[1];
  CHECK(types.types()[nested.params[0]].tag == ir::TypeTag::Enum);
  CHECK(nested.params[0].idx != sig.params[0].idx);
}

TEST_CASE("Check rejects generic arity mismatches") {
  {
    io::TempDir dir =
        io::TempDir::create_unique("alcy_types_generic_bare_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "enum Box<T> { Filled(T), Empty }\n"
                                        "fn f(x: Box) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    io::TempDir dir =
        io::TempDir::create_unique("alcy_types_generic_many_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "enum Box<T> { Filled(T), Empty }\n"
                                        "fn f(x: Box<i32, u8>) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    io::TempDir dir =
        io::TempDir::create_unique("alcy_types_param_scope_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: T) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
}

TEST_CASE("Check constructs generic enums from annotations") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_generic_ctor_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "fn f(x: Box<i32>) -> i32 {\n"
                                      "  ret match x {\n"
                                      "    Box::Filled(v) => v,\n"
                                      "    Box::Empty => 0,\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  b: Box<i32> := Box::Filled(41i32)\n"
                                      "  ret f(b) - 41\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
}

TEST_CASE("Check infers generic constructors from payload arguments") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_infer_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "fn main() -> i32 {\n"
                                      "  b := Box::Filled(41i32)\n"
                                      "  ret match b {\n"
                                      "    Box::Filled(v) => v - 41,\n"
                                      "    Box::Empty => 1,\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects generic constructors with no binding argument") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_infer_bad_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "fn main() -> i32 {\n"
                                      "  b := Box::Empty\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check enforces generic match exhaustiveness") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_generic_exh_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "fn f(x: Box<i32>) -> i32 {\n"
                                      "  ret match x {\n"
                                      "    Box::Filled(v) => v,\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check instantiates generic methods") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_method_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "impl<T> Box<T> {\n"
                                      "  fn is_filled(self: Self) -> bool {\n"
                                      "    ret match self {\n"
                                      "      Box::Filled(_) => true,\n"
                                      "      Box::Empty => false,\n"
                                      "    }\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  b: Box<i32> := Box::Filled(1i32)\n"
                                      "  if b.is_filled() {\n"
                                      "    ret 0\n"
                                      "  }\n"
                                      "  ret 1\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
}

TEST_CASE("Check instantiates generic methods recursively") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_generic_rec_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "enum Box<T> { Filled(T), Empty }\n"
                       "impl<T> Box<T> {\n"
                       "  fn or(self: Self, d: T) -> T {\n"
                       "    ret match self {\n"
                       "      Box::Filled(v) => v,\n"
                       "      Box::Empty => d,\n"
                       "    }\n"
                       "  }\n"
                       "  fn rec(self: Self, n: i32, d: T) -> T {\n"
                       "    if n <= 0 {\n"
                       "      ret self.or(d)\n"
                       "    }\n"
                       "    ret self.rec(n - 1, d)\n"
                       "  }\n"
                       "}\n"
                       "fn main() -> i32 {\n"
                       "  a: Box<i32> := Box::Filled(1i32)\n"
                       "  b: Box<bool> := Box::Filled(true)\n"
                       "  if a.rec(2, 0i32) != 1 {\n"
                       "    ret 1\n"
                       "  }\n"
                       "  if b.rec(2, false) != true {\n"
                       "    ret 2\n"
                       "  }\n"
                       "  ret 0\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
}

TEST_CASE("Check rejects unknown generic methods") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_nomethod_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Box<T> { Filled(T), Empty }\n"
                                      "impl<T> Box<T> {\n"
                                      "  fn is_filled(self: Self) -> bool {\n"
                                      "    ret true\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  b: Box<i32> := Box::Filled(1i32)\n"
                                      "  ret b.missing()\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves cross-module types") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_cross_test_");
  const bool setup = write_all(
      dir,
      {
          {"main.al",
           "use a::Point;\nstruct Holder { p: Point, q: a::Other }\nfn "
           "main() {}\n"},
          {"a.al", "pub struct Point { x: i32 }\nstruct Other { y: bool }\n"},
      });
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al", "a.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  const CheckedModule::NamedType* holder = find_type(*root, "Holder");
  CHECK(holder != nullptr);
  if (holder == nullptr) {
    return;
  }
  CHECK(result.package->types.is_copy_type(holder->type));
}

TEST_CASE("Check instantiates core generic types with dedup") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_core_generic_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: Result<i32, bool>) -> Option<i32> {\n"
                       "  ret Option::None\n"
                       "}\n"
                       "fn g(b: Result<i32, bool>) -> i32 {\n"
                       "  ret 0\n"
                       "}\n"},
                      {kCorePreludeFile, kCorePrelude}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                      ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.size() != 2) {
    return;
  }
  const ir::Storage& types = result.package->types;
  CHECK(types.types()[root->functions[0].params[0]].tag == ir::TypeTag::Enum);
  CHECK(types.types()[root->functions[0].ret].tag == ir::TypeTag::Enum);
  // Identical instantiations share one index.
  CHECK(root->functions[0].params[0].idx == root->functions[1].params[0].idx);
  CHECK(root->functions[0].ret.idx != root->functions[0].params[0].idx);
}

TEST_CASE("Check lets user code define Result and Option") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_shadow_core_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub enum Option<T> { Only(T), Never }\n"
                       "pub enum Result<T, E> { Yes(T), No(E) }\n"
                       "fn main() -> i32 {\n"
                       "  o: Option<i32> := Option::Only(1i32)\n"
                       "  ret match o {\n"
                       "    Option::Only(v) => v - 1,\n"
                       "    Option::Never => 1,\n"
                       "  }\n"
                       "}\n"},
                      {kCorePreludeFile, kCorePrelude}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                      ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
}

TEST_CASE("Check maps pointer widths for sized integers") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_width_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct W { a: isize, b: usize }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture narrow;
  const CheckCase narrow_result =
      check_case(dir, "main.al", {"main.al"}, narrow, ir::PointerWidth::W32);
  CHECK(narrow_result.package.has_value());
  Fixture wide;
  const CheckCase wide_result =
      check_case(dir, "main.al", {"main.al"}, wide, ir::PointerWidth::W64);
  CHECK(wide_result.package.has_value());
  if (!narrow_result.package.has_value() || !wide_result.package.has_value()) {
    return;
  }
  const CheckedModule* narrow_root = find_checked(*narrow_result.package, "");
  const CheckedModule* wide_root = find_checked(*wide_result.package, "");
  CHECK(narrow_root != nullptr);
  CHECK(wide_root != nullptr);
  if (narrow_root == nullptr || wide_root == nullptr) {
    return;
  }
  const CheckedModule::NamedType* narrow_w = find_type(*narrow_root, "W");
  const CheckedModule::NamedType* wide_w = find_type(*wide_root, "W");
  CHECK(narrow_w != nullptr);
  CHECK(wide_w != nullptr);
  if (narrow_w == nullptr || wide_w == nullptr) {
    return;
  }
  const ir::StructType& narrow_struct =
      narrow_result.package->types.struct_types()
          [narrow_result.package->types.types()[narrow_w->type].as_struct()];
  const ir::StructType& wide_struct =
      wide_result.package->types.struct_types()
          [wide_result.package->types.types()[wide_w->type].as_struct()];
  CHECK(narrow_result.package->types.types()[narrow_struct.fields[0]].tag ==
        ir::TypeTag::I32);
  CHECK(wide_result.package->types.types()[wide_struct.fields[0]].tag ==
        ir::TypeTag::I64);
}

TEST_CASE("Check rejects unknown types") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_unknown_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Holder { p: Nope }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects value-recursive types") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_recursive_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct A { b: B }\n"
                                      "struct B { a: A }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts reference cycles") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_refcycle_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct A { r: &B }\n"
                                      "struct B { r: &A }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects duplicate definitions") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_dup_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Foo { x: i32 }\n"
                                      "struct Foo { y: bool }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects malformed generics") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_types_arity_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: Result<i32>) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_types_generic_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "struct Box { x: i32 }\n"
                                        "fn f(x: Box<i32>) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
}

TEST_CASE("Check accepts mutable reference fields as move-only") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_mutfield_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Holder { r: &mut i32, s: &i32 }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  const CheckedModule::NamedType* holder =
      root != nullptr ? find_type(*root, "Holder") : nullptr;
  CHECK(holder != nullptr);
  if (holder == nullptr) {
    return;
  }
  CHECK(!result.package->types.is_copy_type(holder->type));
}

TEST_CASE("Check exposes core generic shapes through the IR") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_registry_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: Result<i32, bool>) -> Option<i32> {\n"
                       "  ret Option::None\n"
                       "}\n"},
                      {kCorePreludeFile, kCorePrelude}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                      ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.empty()) {
    return;
  }
  const ir::Storage& types = result.package->types;
  const ir::TypeIdx result_ty = root->functions[0].params[0];
  const ir::TypeIdx option_ty = root->functions[0].ret;
  const ir::EnumType& result_shape =
      types.enum_types()[types.types()[result_ty].as_enum()];
  const ir::EnumType& option_shape =
      types.enum_types()[types.types()[option_ty].as_enum()];
  CHECK(result_shape.variants.size() == 2);
  CHECK(option_shape.variants.size() == 2);
  const ir::EnumVariantType& ok =
      types.enum_variant_types()[result_shape.variants.head()];
  const ir::EnumVariantType& err =
      types.enum_variant_types()[ir::EnumVariantTypeIdx(
          result_shape.variants.head().idx + 1)];
  CHECK(ok.fields.size() == 1);
  CHECK(err.fields.size() == 1);
  CHECK(types.types()[ok.fields[0]].tag == ir::TypeTag::I32);
  CHECK(types.types()[err.fields[0]].tag == ir::TypeTag::I1);
  const ir::EnumVariantType& some =
      types.enum_variant_types()[option_shape.variants.head()];
  const ir::EnumVariantType& none =
      types.enum_variant_types()[ir::EnumVariantTypeIdx(
          option_shape.variants.head().idx + 1)];
  CHECK(some.fields.size() == 1);
  CHECK(none.fields.empty());
  CHECK(types.types()[some.fields[0]].tag == ir::TypeTag::I32);
}

TEST_CASE("Check judges Copy structurally") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_copy_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct AllCopy { a: i32, b: &i32, c: (bool, str) }\n"
                       "struct HasMut { r: &mut i32 }\n"
                       "enum Mixed { A(i32), B(&mut bool) }\n"
                       "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  const CheckedModule::NamedType* all_copy = find_type(*root, "AllCopy");
  const CheckedModule::NamedType* has_mut = find_type(*root, "HasMut");
  const CheckedModule::NamedType* mixed = find_type(*root, "Mixed");
  CHECK(all_copy != nullptr);
  CHECK(has_mut != nullptr);
  CHECK(mixed != nullptr);
  if (all_copy == nullptr || has_mut == nullptr || mixed == nullptr) {
    return;
  }
  CHECK(result.package->types.is_copy_type(all_copy->type));
  CHECK(!result.package->types.is_copy_type(has_mut->type));
  CHECK(!result.package->types.is_copy_type(mixed->type));
}

TEST_CASE("Check expressions accept well-typed programs") {
  io::TempDir dir = io::TempDir::create_unique("alcy_expr_ok_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Point { x: i32, y: i32 }\n"
                                      "fn add(a: i32, b: i32) -> i32 {\n"
                                      "  ret a + b\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  x: u8 := 42u8\n"
                                      "  y := x + 1\n"
                                      "  p := Point { x: 1, y: 2 }\n"
                                      "  t := (1, true)\n"
                                      "  c := 1 as u64\n"
                                      "  d := 1.5 + 2.0\n"
                                      "  print(\"hi\")\n"
                                      "  _ := y\n"
                                      "  _ := p\n"
                                      "  _ := t\n"
                                      "  _ := c\n"
                                      "  _ := d\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check expressions reject mismatches") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_expr_suffix_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  x: u8 := 42i32\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_expr_binop_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  x := 1 + true\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_expr_ret_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f() -> i32 {\n"
                                        "  ret true\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_expr_call_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn add(a: i32, b: i32) -> i32 {\n"
                                        "  ret a + b\n"
                                        "}\n"
                                        "fn main() {\n"
                                        "  _ := add(1)\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_expr_field_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "struct Point { x: i32 }\n"
                                        "fn main() {\n"
                                        "  p := Point { x: 1 }\n"
                                        "  _ := p.y\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check question-mark propagation") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_question_ok_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn get() -> Result<i32, bool> {\n"
                                        "  ret Result::Ok(1i32)\n"
                                        "}\n"
                                        "fn caller() -> Result<i32, bool> {\n"
                                        "  x := get()?\n"
                                        "  ret Result::Ok(x + 1i32)\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir =
        io::TempDir::create_unique("alcy_question_mismatch_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn get() -> Result<i32, bool> {\n"
                                        "  ret Result::Ok(1i32)\n"
                                        "}\n"
                                        "fn caller() -> Result<i32, str> {\n"
                                        "  x := get()?\n"
                                        "  ret Result::Ok(x + 1i32)\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_question_plain_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn get() -> Result<i32, bool> {\n"
                                        "  ret Result::Ok(1i32)\n"
                                        "}\n"
                                        "fn caller() -> i32 {\n"
                                        "  ret get()?\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check question-mark works on any enum") {
  io::TempDir dir = io::TempDir::create_unique("alcy_question_user_enum_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Early<T> { Value(T), Stop }\n"
                                      "fn lookup(x: i32) -> Early<i32> {\n"
                                      "  if x > 0 {\n"
                                      "    ret Early::Value(x)\n"
                                      "  }\n"
                                      "  ret Early::Stop\n"
                                      "}\n"
                                      "fn caller(x: i32) -> Early<i32> {\n"
                                      "  v := lookup(x)?\n"
                                      "  ret Early::Value(v + 1i32)\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  ret match caller(1i32) {\n"
                                      "    Early::Value(v) => v - 2i32,\n"
                                      "    Early::Stop => 1i32,\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check match exhaustiveness") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_bool_ok_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(b: bool) -> i32 {\n"
                                        "  ret match b {\n"
                                        "    true => 1,\n"
                                        "    false => 0,\n"
                                        "  }\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_bool_bad_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(b: bool) -> i32 {\n"
                                        "  ret match b {\n"
                                        "    true => 1,\n"
                                        "  }\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_enum_ok_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "enum Choice { Yes, No(i32) }\n"
                                        "fn f(c: Choice) -> i32 {\n"
                                        "  ret match c {\n"
                                        "    Yes => 1,\n"
                                        "    No(x) => x,\n"
                                        "  }\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_enum_bad_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "enum Choice { Yes, No(i32) }\n"
                                        "fn f(c: Choice) -> i32 {\n"
                                        "  ret match c {\n"
                                        "    Yes => 1,\n"
                                        "  }\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_int_wild_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: i32) -> i32 {\n"
                                        "  ret match x {\n"
                                        "    0 => 1,\n"
                                        "    _ => 0,\n"
                                        "  }\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_option_ok_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(o: Option<i32>) -> i32 {\n"
                                        "  ret match o {\n"
                                        "    Option::Some(x) => x,\n"
                                        "    Option::None => 0,\n"
                                        "  }\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_option_bad_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(o: Option<i32>) -> i32 {\n"
                                        "  ret match o {\n"
                                        "    Option::Some(x) => x,\n"
                                        "  }\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_match_int_bad_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: i32) -> i32 {\n"
                                        "  ret match x {\n"
                                        "    0 => 1,\n"
                                        "  }\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check or-patterns bind shared names") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_or_same_test_");
    const bool setup =
        write_all(dir, {{"main.al",
                         "enum Shape { Circle(i32), Square(i32), Rect }\n"
                         "fn f(s: Shape) -> i32 {\n"
                         "  ret match s {\n"
                         "    Shape::Circle(x) | Shape::Square(x) => x,\n"
                         "    Shape::Rect => 0,\n"
                         "  }\n"
                         "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_or_mismatch_test_");
    const bool setup =
        write_all(dir, {{"main.al",
                         "enum Shape { Circle(i32), Square(i32), Rect }\n"
                         "fn f(s: Shape) -> i32 {\n"
                         "  ret match s {\n"
                         "    Shape::Circle(x) | Shape::Rect => x,\n"
                         "    Shape::Square(y) => y,\n"
                         "  }\n"
                         "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check inherent and core generic methods") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_method_ok_test_");
    const bool setup =
        write_all(dir, {{"main.al",
                         "struct Point { x: i32 }\n"
                         "impl Point {\n"
                         "  fn get(self: &Self) -> i32 {\n"
                         "    ret self.x\n"
                         "  }\n"
                         "}\n"
                         "fn f(r: Result<i32, bool>) -> i32 {\n"
                         "  p := Point { x: 1 }\n"
                         "  v := r.unwrap()\n"
                         "  ok := r.is_ok()\n"
                         "  _ := ok\n"
                         "  ret p.get() + v\n"
                         "}\n"},
                        {kCorePreludeFile,
                         R"(pub intrinsic fn panic(msg: str) -> !;
pub enum Option<T> { Some(T), None }
pub enum Result<T, E> { Ok(T), Err(E) }
impl<T, E> Result<T, E> {
  fn is_ok(self: Self) -> bool { ret true }
  fn is_err(self: Self) -> bool { ret false }
  fn unwrap(self: Self) -> T { ret panic("stub") }
  fn expect(self: Self, msg: str) -> T { ret panic(msg) }
}
)"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir =
        io::TempDir::create_unique("alcy_method_missing_core_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(r: Result<i32, bool>) -> i32 {\n"
                                        "  ret r.no_such_method()\n"
                                        "}\n"},
                                       {kCorePreludeFile, kCorePrelude}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                        ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_method_bad_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "struct Point { x: i32 }\n"
                                        "fn f() {\n"
                                        "  p := Point { x: 1 }\n"
                                        "  _ := p.missing()\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check unused-value warnings") {
  io::TempDir dir = io::TempDir::create_unique("alcy_mustuse_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn get() -> Result<i32, bool> {\n"
                                      "  ret Result::Ok(1i32)\n"
                                      "}\n"
                                      "fn plain() -> i32 {\n"
                                      "  ret 1\n"
                                      "}\n"
                                      "fn side() {\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := get()\n"
                                      "  _ := plain()\n"
                                      "  get()\n"
                                      "  side()\n"
                                      "}\n"},
                                     {kCorePreludeFile, kCorePrelude}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f,
                                      ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
  // The bare `get()` statement warns; `_ :=` discards and `()`
  // statements do not.
  CHECK(f.bag.warning_count() == 1);
}

TEST_CASE("Check items enforce entry and initializer rules") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_main_bad_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() -> bool {\n"
                                        "  ret true\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_static_mut_test_");
    const bool setup =
        write_all(dir, {{"main.al", "static r: &mut i32 = 0\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_const_call_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn one() -> i32 {\n"
                                        "  ret 1\n"
                                        "}\n"
                                        "const k: i32 = one()\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_let_refutable_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  0 := 1\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_range_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  _ := 1..10\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_break_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  break\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check borrow expressions") {
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_borrow_ok_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  x := 1\n"
                                        "  r: &i32 := &x\n"
                                        "  m: &mut i32 := &mut x\n"
                                        "  _ := r\n"
                                        "  _ := m\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    io::TempDir dir = io::TempDir::create_unique("alcy_borrow_mismatch_test_");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  x := 1\n"
                                        "  r: &i32 := &mut x\n"
                                        "  _ := r\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check accepts comp declarations and blocks") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_comp_ok_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn double(comp n: i32) -> i32 {\n"
                                      "  ret n * 2\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  comp k := 21\n"
                                      "  _ := double(k)\n"
                                      "  _ := comp { 1 + 2 }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects runtime arguments for comp parameters") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_comp_arg_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn double(comp n: i32) -> i32 {\n"
                                      "  ret n * 2\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  x := 21\n"
                                      "  _ := double(x)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects non-comp-known comp initializers") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_comp_init_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  comp k := x\n"
                                      "  _ := k\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects ret inside comp blocks") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_comp_ret_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  ret comp { ret 1 }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects print inside comp blocks") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_comp_io_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := comp { print(\"hi\") }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts memcopy intrinsic declarations") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_intrinsic_ok_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn memcopy(dst: &mut u8, "
                                      "src: &u8, n: usize);\n"
                                      "fn main() {\n"
                                      "  mut a := 1u8\n"
                                      "  b := 2u8\n"
                                      "  memcopy(&mut a, &b, 1)\n"
                                      "  _ := a\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects unknown intrinsics") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_intrinsic_unknown_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn frobnicate(x: i32);\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects mistyped intrinsic signatures") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_intrinsic_sig_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn memcopy(x: i32);\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects comp parameters on intrinsics") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_intrinsic_comp_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn memcopy(dst: &mut u8, "
                                      "src: &u8, comp n: usize);\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves prelude calls without imports") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_prelude_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := help()\n"
                                      "}\n"},
                                     {"core.al",
                                      "pub fn help() -> i32 {\n"
                                      "  ret 1\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result =
      check_case(dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64,
                 {{"core", "core.al"}});
  CHECK(result.package.has_value());
}

TEST_CASE("Check accepts string intrinsic declarations") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_str_intrinsic_test_");
  const bool setup = write_all(
      dir,
      {{"main.al",
        "intrinsic fn str_len(s: str) -> usize;\n"
        "intrinsic fn str_byte(s: str, i: usize) -> u8;\n"
        "intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;\n"
        "fn main() {\n"
        "  s := \"hi\"\n"
        "  _ := str_len(s)\n"
        "  _ := str_byte(s, 0)\n"
        "  _ := str_slice(s, 0, 2)\n"
        "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects mistyped string intrinsic signatures") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_str_intrinsic_sig_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn str_len(s: str) -> i32;\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts array construction and indexing") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_array_ok_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  mut buf := [0u8; 4]\n"
                                      "  buf[0] = 1u8\n"
                                      "  _ := buf[0]\n"
                                      "  _ := [1i32, 2i32]\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects heterogeneous array literals") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_array_hetero_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := [1i32, true]\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects oversized array repeats") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_array_huge_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := [0u8; 9999999999]\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects fmt arity mismatches") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_fmt_arity_test_");
  const bool setup = write_all(
      dir, {{"main.al",
             "fn main() {\n"
             "  mut buf := [0u8; 8]\n"
             "  _ := write(\"a={} b={}\", &mut buf, (1i32,))\n"
             "}\n"},
            {"core.al",
             "pub struct WriteOutcome { written: usize, total: usize }\n"
             "pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> "
             "WriteOutcome {\n"
             "  panic(\"x\")\n"
             "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result =
      check_case(dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64,
                 {{"core", "core.al"}});
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects non-tuple fmt arguments") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_fmt_tuple_test_");
  const bool setup = write_all(
      dir, {{"main.al",
             "fn main() {\n"
             "  mut buf := [0u8; 8]\n"
             "  _ := write(\"a={}\", &mut buf, 1i32)\n"
             "}\n"},
            {"core.al",
             "pub struct WriteOutcome { written: usize, total: usize }\n"
             "pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> "
             "WriteOutcome {\n"
             "  panic(\"x\")\n"
             "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result =
      check_case(dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64,
                 {{"core", "core.al"}});
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts generic free functions and turbofish arguments") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_generic_fn_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn id<T>(x: T) -> T {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn pair<T>(a: T, b: T) -> T {\n"
                                      "  ret a\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  a := id(1i32)\n"
                                      "  b := id::<i32>(2i32)\n"
                                      "  c := pair(3i32, 4i32)\n"
                                      "  _ := id(true)\n"
                                      "  ret a + b + c - 10\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects uninferable generic call arguments") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_generic_fn_unbound_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn make<T>() -> T {\n"
                                      "  panic(\"x\")\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := make()\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts typed heap intrinsics") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_typed_heap_test_");
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub intrinsic fn alloc<T>(count: "
                       "usize) -> &mut MaybeUninit<T>;\n"
                       "pub intrinsic fn dealloc<T>(ptr: &mut "
                       "MaybeUninit<T>, count: usize);\n"
                       "pub intrinsic fn size_of<T>() -> "
                       "usize;\n"
                       "pub intrinsic fn align_of<T>() -> "
                       "usize;\n"
                       "pub intrinsic fn elem_ptr<T>(ptr: &mut "
                       "MaybeUninit<T>, index: usize) -> &mut "
                       "MaybeUninit<T>;\n"
                       "pub intrinsic fn uninit_write<T>(slot: "
                       "&mut MaybeUninit<T>, value: T);\n"
                       "pub intrinsic fn uninit_assume<T>(slot: "
                       "&mut MaybeUninit<T>) -> &mut T;\n"
                       "fn main() {\n"
                       "  data := alloc::<i32>(4)\n"
                       "  uninit_write(elem_ptr(data, 0), 1i32)\n"
                       "  _ := *uninit_assume(elem_ptr(data, 0))\n"
                       "  _ := size_of::<i32>()\n"
                       "  _ := align_of::<i32>()\n"
                       "  dealloc(data, 4)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects reading an uninitialized slot") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_uninit_read_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "pub intrinsic fn alloc<T>(count: "
                                      "usize) -> &mut MaybeUninit<T>;\n"
                                      "pub intrinsic fn elem_ptr<T>(ptr: &mut "
                                      "MaybeUninit<T>, index: usize) -> &mut "
                                      "MaybeUninit<T>;\n"
                                      "fn main() -> i32 {\n"
                                      "  data := alloc::<i32>(1)\n"
                                      "  ret *elem_ptr(data, 0)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects declaring MaybeUninit") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_uninit_shadow_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct MaybeUninit<T> { value: T }\n"
                                      "fn main() {\n"
                                      "  _ := MaybeUninit { value: 1i32 }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects an intrinsic declared with the wrong shape") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_intrinsic_shape_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "pub intrinsic fn elem_ptr<T>(ptr: "
                                      "&MaybeUninit<T>, index: usize) -> "
                                      "&MaybeUninit<T>;\n"
                                      "fn main() {\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects dereferencing a non-reference") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_deref_value_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  x := 1i32\n"
                                      "  _ := *x\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects assignment through a shared reference") {
  io::TempDir dir = io::TempDir::create_unique("alcy_types_deref_shared_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn bump(p: &i32) {\n"
                                      "  *p = 1\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves an associated function of a generic type") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_assoc_generic_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Box<T> { item: T }\n"
                                      "impl<T> Box<T> {\n"
                                      "  fn empty() -> Box<T> {\n"
                                      "    ret Box { item: 0i32 }\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  b := Box::<i32>::empty()\n"
                                      "  ret b.item\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE(
    "Check rejects an associated function of a generic type without "
    "type arguments") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_types_assoc_no_args_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Box<T> { item: T }\n"
                                      "impl<T> Box<T> {\n"
                                      "  fn empty() -> Box<T> {\n"
                                      "    ret Box { item: 0i32 }\n"
                                      "  }\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  b := Box::empty()\n"
                                      "  ret b.item\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckCase result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

}  // namespace analyzer
