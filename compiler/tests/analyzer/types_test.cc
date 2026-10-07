// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/types.h"

#include <deque>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/diag_code.h"
#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/idx.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "i18n/language.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace analyzer {

namespace {

struct Fixture {
  // Resolution runs through the pipeline's parse, which is where the
  // items come from, so the context owns the arena, the bag, and the
  // interner the cases inspect. `name_capacity` sizes the shared name
  // table; zero takes its default, and a case that has to spend it asks
  // for less.
  explicit Fixture(usize name_capacity = 0)
      : ctx{i18n::Language::EnUs, ast::AstArena::DEFAULT_SPAN_CAPACITY,
            name_capacity} {}
  pipeline::PipelineContext ctx;
  ast::AstArena& ast = ctx.ast;
  diag::DiagBag& bag = ctx.bag;
  source::SourceManager& sources = ctx.sources;
  str::StringInterner& strings = ctx.strings;
};

// The sources one case declares, held in memory. It stands in for a
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

struct CheckOutcome {
  std::optional<CheckedPackage> package;
};

CheckOutcome check_case(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    Fixture& f,
    ir::PointerWidth width = ir::PointerWidth::W64,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude = {}) {
  std::vector<ModuleInput> inputs;
  source::FileId root = source::UNKNOWN_FILE;
  for (std::string_view rel : rels) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    const source::FileId id = f.sources.add_virtual(file->name, file->bytes);
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
  std::deque<std::string> prelude_storage;
  std::vector<ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    const source::FileId id = f.sources.add_virtual(file->name, file->bytes);
    prelude_storage.emplace_back(name);
    // A staged prelude source is a package facade, so its public
    // surface is in scope without a `use`; see
    // docs/adr/0016-suites-and-the-std-split.md.
    prelude_inputs.push_back({prelude_storage.back(), id, true});
  }
  base::Result<ModuleTree, diag::Reported> tree_result =
      pipeline::resolve_inputs(f.ctx, root, inputs, "testpkg", prelude_inputs);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt};
  }
  ModuleTree tree = std::move(tree_result).unwrap();
  base::Result<CheckedPackage, diag::Reported> checked_result =
      check_package(tree, width, f.ast, f.bag, f.strings);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt};
  }
  return {std::move(checked_result).unwrap()};
}

// Resolves a root and staged facades, then checks under a package
// policy table the caller supplies: the seal cases need rows
// resolve_inputs does not build.
CheckOutcome check_with_policies(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude,
    std::span<const PackagePolicy> policies,
    Fixture& f) {
  const tests::VirtualSource* const root_file = dir.find(root_rel);
  if (root_file == nullptr) {
    return {std::nullopt};
  }
  const source::FileId root =
      f.sources.add_virtual(root_file->name, root_file->bytes);
  const ModuleInput root_input{"", root};
  std::deque<std::string> prelude_storage;
  std::vector<ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    const source::FileId id = f.sources.add_virtual(file->name, file->bytes);
    prelude_storage.emplace_back(name);
    prelude_inputs.push_back({prelude_storage.back(), id, true});
  }
  base::Result<ModuleTree, diag::Reported> tree_result =
      pipeline::resolve_inputs(f.ctx, root, {&root_input, 1}, "testpkg",
                               prelude_inputs);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt};
  }
  ModuleTree tree = std::move(tree_result).unwrap();
  tree.package_policies = policies;
  base::Result<CheckedPackage, diag::Reported> checked_result =
      check_package(tree, ir::PointerWidth::W64, f.ast, f.bag, f.strings);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt};
  }
  return {std::move(checked_result).unwrap()};
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

// `Option` and `Result` live in the core prelude since
// `docs/adr/0009-result-option-library-enums.md`, so
// tests that mention them inject a matching declaration set.
constexpr std::string_view CORE_PRELUDE =
    R"(pub intrinsic fn panic(msg: str) -> !;
pub fn print(msg: str) {}
pub enum Option<T> { Some(T), None }
pub enum Result<T, E> { Ok(T), Err(E) }
)";

constexpr std::string_view CORE_PRELUDE_FILE = "core.al";

// Static storage keeps the returned initializer_list valid for the
// caller's use; an initializer_list of temporaries would dangle.
const std::initializer_list<std::pair<std::string_view, std::string_view>>&
core_prelude() {
  static const std::initializer_list<
      std::pair<std::string_view, std::string_view>>
      PRELUDE = {{"core", CORE_PRELUDE_FILE}};
  return PRELUDE;
}

}  // namespace

TEST_CASE("Check interns structs with named fields") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Point { x: i32, y: i32 }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
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
  CHECK(result.package->types->types()[point->type].tag == ir::TypeTag::Struct);
  CHECK(result.package->types->is_copy_type(point->type));
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
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Choice { Yes, No(i32) }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
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
  CHECK(result.package->types->types()[choice->type].tag == ir::TypeTag::Enum);
  CHECK(result.package->types->is_copy_type(choice->type));
}

TEST_CASE("Check instantiates generic structs") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.size() < 2) {
    return;
  }
  const ir::Storage& types = *result.package->types;
  const ir::TypeIdx pair_i_s = root->functions[0].params[0];
  CHECK(types.types()[pair_i_s].tag == ir::TypeTag::Struct);
  // Identical instantiations share one index; different ones do not.
  CHECK(pair_i_s.idx == root->functions[0].ret.idx);
  CHECK(pair_i_s.idx != root->functions[1].params[0].idx);
}

TEST_CASE("Check keeps generic struct field copies contiguous") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct Leaf<T> { slot: T }\n"
                       "struct Pair<S, T> { first: Leaf<S>, second: Leaf<T> }\n"
                       "fn f(x: Pair<i32, u8>) -> i32 {\n"
                       "  ret 0\n"
                       "}\n"
                       "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.empty()) {
    return;
  }
  // The first field's `Leaf<i32>` instantiation creates types of its
  // own while the fields resolve; the copies that follow must still
  // form one contiguous range.
  const ir::Storage& types = *result.package->types;
  const ir::TypeIdx pair_ty = root->functions[0].params[0];
  CHECK(types.types()[pair_ty].tag == ir::TypeTag::Struct);
  if (types.types()[pair_ty].tag != ir::TypeTag::Struct) {
    return;
  }
  const ir::StructType& pair_shape =
      types.struct_types()[types.types()[pair_ty].as_struct()];
  CHECK(pair_shape.fields.size() == 2);
  if (pair_shape.fields.size() != 2) {
    return;
  }
  CHECK(pair_shape.fields[0].idx + 1 == pair_shape.fields[1].idx);
  CHECK(types.types()[pair_shape.fields[0]].tag == ir::TypeTag::Struct);
  CHECK(types.types()[pair_shape.fields[1]].tag == ir::TypeTag::Struct);
}

TEST_CASE("Check publishes a generic instance nested in a field") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Leaf<T> { slot: T }\n"
                                      "struct Wrap<T> { only: Leaf<T> }\n"
                                      "fn f(x: Wrap<i32>) -> i32 {\n"
                                      "  ret 0\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr || root->functions.empty()) {
    return;
  }
  // Instantiating `Wrap<i32>` instantiates `Leaf<i32>` while the field
  // resolves; the outer instance is the one completed, so its fields
  // publish for lowering lookups.
  const ir::TypeIdx wrap_ty = root->functions[0].params[0];
  bool wrap_published = false;
  bool leaf_published = false;
  for (const CheckedModule::StructInfo& info : root->structs) {
    if (info.type.idx == wrap_ty.idx) {
      wrap_published = info.fields.size() == 1 && info.fields[0] == "only";
    }
    if (info.fields.size() == 1 && info.fields[0] == "slot") {
      leaf_published = true;
    }
  }
  CHECK(wrap_published);
  CHECK(leaf_published);
}

TEST_CASE("Check keeps outer type parameters across nested instantiation") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn id<A>(x: A) -> A {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn keep<T>(x: T) -> T {\n"
                                      "  _ := id::<u8>(7 as u8)\n"
                                      "  z: T := x\n"
                                      "  ret z\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  ret keep(42i32) - 42\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  // Binding `id`'s parameter must not replace `keep`'s scope: `T` still
  // names the outer argument in the annotation that follows.
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check instantiates generic struct methods") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects arity mismatch on generic structs") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves annotations and signatures") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
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
  const ir::Storage& types = *result.package->types;
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
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
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
  const ir::Storage& types = *result.package->types;
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
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: T) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
}

TEST_CASE("Check constructs generic enums from annotations") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
}

TEST_CASE("Check unifies tuples holding generic instances") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "enum Maybe<T> { Yes(T), No }\n"
                                      "struct Holder<T> { v: T }\n"
                                      "fn f() -> Holder<(Maybe<i32>, i32)> {\n"
                                      "  ret Holder { v: (Maybe::Yes(1), 2) }\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check infers generic constructors from payload arguments") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects generic constructors with no binding argument") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check enforces generic match exhaustiveness") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check instantiates generic methods") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
}

TEST_CASE("Check instantiates generic methods recursively") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
}

TEST_CASE("Check rejects unknown generic methods") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves cross-module types") {
  VirtualDir dir;
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
  const CheckOutcome result =
      check_case(dir, "main.al", {"main.al", "a.al"}, f);
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
  CHECK(result.package->types->is_copy_type(holder->type));
}

TEST_CASE("Check instantiates core generic types with dedup") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: Result<i32, bool>) -> Option<i32> {\n"
                       "  ret Option::None\n"
                       "}\n"
                       "fn g(b: Result<i32, bool>) -> i32 {\n"
                       "  ret 0\n"
                       "}\n"},
                      {CORE_PRELUDE_FILE, CORE_PRELUDE}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f,
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
  const ir::Storage& types = *result.package->types;
  CHECK(types.types()[root->functions[0].params[0]].tag == ir::TypeTag::Enum);
  CHECK(types.types()[root->functions[0].ret].tag == ir::TypeTag::Enum);
  // Identical instantiations share one index.
  CHECK(root->functions[0].params[0].idx == root->functions[1].params[0].idx);
  CHECK(root->functions[0].ret.idx != root->functions[0].params[0].idx);
}

TEST_CASE("Check lets user code define Result and Option") {
  VirtualDir dir;
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
                      {CORE_PRELUDE_FILE, CORE_PRELUDE}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f,
                                         ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
}

TEST_CASE("Check maps pointer widths for sized integers") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct W { a: isize, b: usize }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture narrow;
  const CheckOutcome narrow_result =
      check_case(dir, "main.al", {"main.al"}, narrow, ir::PointerWidth::W32);
  CHECK(narrow_result.package.has_value());
  Fixture wide;
  const CheckOutcome wide_result =
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
      narrow_result.package->types->struct_types()
          [narrow_result.package->types->types()[narrow_w->type].as_struct()];
  const ir::StructType& wide_struct =
      wide_result.package->types->struct_types()
          [wide_result.package->types->types()[wide_w->type].as_struct()];
  CHECK(narrow_result.package->types->types()[narrow_struct.fields[0]].tag ==
        ir::TypeTag::I32);
  CHECK(wide_result.package->types->types()[wide_struct.fields[0]].tag ==
        ir::TypeTag::I64);
}

TEST_CASE("Check rejects unknown types") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Holder { p: Nope }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects value-recursive types") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct A { b: B }\n"
                                      "struct B { a: A }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts reference cycles") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct A { r: &B }\n"
                                      "struct B { r: &A }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects duplicate definitions") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Foo { x: i32 }\n"
                                      "struct Foo { y: bool }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a duplicate free function") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn foo() -> i32 { ret 1 }\n"
                                      "fn foo() -> i32 { ret 2 }\n"
                                      "fn main() -> i32 { ret foo() }\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a duplicate method across impl blocks") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct S { n: i32 }\n"
                       "impl S {\n"
                       "  fn get(self: &Self) -> i32 { ret self.n }\n"
                       "}\n"
                       "impl S {\n"
                       "  fn get(self: &Self) -> i32 { ret self.n }\n"
                       "}\n"
                       "fn main() -> i32 { ret S { n: 1 }.get() }\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check keeps a free function beside a method of the same name") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct S { n: i32 }\n"
                       "impl S {\n"
                       "  fn get(self: &Self) -> i32 { ret self.n }\n"
                       "}\n"
                       "fn get() -> i32 { ret 7 }\n"
                       "fn main() -> i32 { ret get() + S { n: 1 }.get() }\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check rejects malformed generics") {
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: Result<i32>) -> i32 {\n"
                                        "  ret 0\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
}

TEST_CASE("Check accepts mutable reference fields as move-only") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Holder { r: &mut i32, s: &i32 }\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
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
  CHECK(!result.package->types->is_copy_type(holder->type));
}

TEST_CASE("Check resolves a slice behind a reference") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn first(v: &[i32]) -> i32 {\n"
                                      "  ret v[0]\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  mut a := [1i32, 2i32]\n"
                                      "  ret first(&a) - 1\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const CheckedModule* root = find_checked(*result.package, "");
  CHECK(root != nullptr);
  if (root == nullptr) {
    return;
  }
  const CheckedModule::FnSig* first = nullptr;
  for (const CheckedModule::FnSig& fn : root->functions) {
    if (fn.name == "first") {
      first = &fn;
      break;
    }
  }
  CHECK(first != nullptr);
  if (first == nullptr) {
    return;
  }
  const ir::Storage& types = *result.package->types;
  CHECK(types.types()[first->params[0]].tag == ir::TypeTag::Ref);
  const ir::TypeIdx pointee =
      types.ref_types()[types.types()[first->params[0]].as_ref()].pointee;
  CHECK(types.types()[pointee].tag == ir::TypeTag::Slice);
  const ir::SliceType& slice =
      types.slice_types()[types.types()[pointee].as_slice()];
  CHECK(types.types()[slice.element].tag == ir::TypeTag::I32);
}

TEST_CASE("Check rejects a bare slice type") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f(v: [i32]) {}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check exposes core generic shapes through the IR") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: Result<i32, bool>) -> Option<i32> {\n"
                       "  ret Option::None\n"
                       "}\n"},
                      {CORE_PRELUDE_FILE, CORE_PRELUDE}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f,
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
  const ir::Storage& types = *result.package->types;
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
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
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
  CHECK(result.package->types->is_copy_type(all_copy->type));
  CHECK(!result.package->types->is_copy_type(has_mut->type));
  CHECK(!result.package->types->is_copy_type(mixed->type));
}

TEST_CASE("Check expressions accept well-typed programs") {
  VirtualDir dir;
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
                                      "}\n"},
                                     {CORE_PRELUDE_FILE, CORE_PRELUDE}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f,
                                         ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
}

TEST_CASE("Check expressions reject mismatches") {
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  x: u8 := 42i32\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  x := 1 + true\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f() -> i32 {\n"
                                        "  ret true\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check question-mark propagation") {
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn get() -> Result<i32, bool> {\n"
                                        "  ret Result::Ok(1i32)\n"
                                        "}\n"
                                        "fn caller() -> Result<i32, bool> {\n"
                                        "  x := get()?\n"
                                        "  ret Result::Ok(x + 1i32)\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn get() -> Result<i32, bool> {\n"
                                        "  ret Result::Ok(1i32)\n"
                                        "}\n"
                                        "fn caller() -> Result<i32, str> {\n"
                                        "  x := get()?\n"
                                        "  ret Result::Ok(x + 1i32)\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn get() -> Result<i32, bool> {\n"
                                        "  ret Result::Ok(1i32)\n"
                                        "}\n"
                                        "fn caller() -> i32 {\n"
                                        "  ret get()?\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check question-mark works on any enum") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check match exhaustiveness") {
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(o: Option<i32>) -> i32 {\n"
                                        "  ret match o {\n"
                                        "    Option::Some(x) => x,\n"
                                        "    Option::None => 0,\n"
                                        "  }\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(o: Option<i32>) -> i32 {\n"
                                        "  ret match o {\n"
                                        "    Option::Some(x) => x,\n"
                                        "  }\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check or-patterns bind shared names") {
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check inherent and core generic methods") {
  {
    VirtualDir dir;
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
                        {CORE_PRELUDE_FILE,
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
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(r: Result<i32, bool>) -> i32 {\n"
                                        "  ret r.no_such_method()\n"
                                        "}\n"},
                                       {CORE_PRELUDE_FILE, CORE_PRELUDE}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(
        dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64, core_prelude());
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check unused-value warnings") {
  VirtualDir dir;
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
                                     {CORE_PRELUDE_FILE, CORE_PRELUDE}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f,
                                         ir::PointerWidth::W64, core_prelude());
  CHECK(result.package.has_value());
  // Only the bare get() statement warns; _ := and void calls do not.
  CHECK(f.bag.warning_count() == 1);
}

TEST_CASE("Check items enforce entry and initializer rules") {
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() -> bool {\n"
                                        "  ret true\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup =
        write_all(dir, {{"main.al", "static r: &mut i32 = 0\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    // A const initializer is a compile-time value, not a literal: a
    // pure call evaluates during compilation.
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
    CHECK(!f.bag.has_errors());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al", "const k: i32 = \"x\"\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  0 := 1\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  _ := 1..<10\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
  {
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  break\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
  }
}

TEST_CASE("Check borrow expressions") {
  {
    VirtualDir dir;
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
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
  {
    // `&mut T` where `&T` is expected is a shared reborrow, so this
    // binds rather than mismatching. See
    // docs/adr/0012-reborrow-on-reference-read.md rule 3.
    VirtualDir dir;
    const bool setup = write_all(dir, {{"main.al",
                                        "fn main() {\n"
                                        "  mut x := 1\n"
                                        "  r: &i32 := &mut x\n"
                                        "  _ := *r\n"
                                        "  x = 2\n"
                                        "  _ := x\n"
                                        "}\n"}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(result.package.has_value());
  }
}

TEST_CASE("Check accepts comp declarations and blocks") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects runtime arguments for comp parameters") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects non-comp-known comp initializers") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects ret inside comp blocks") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  ret comp { ret 1 }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects print inside comp blocks") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := comp { print(\"hi\") }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts memcopy intrinsic declarations") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "unsafe intrinsic fn memcopy(dst: &mut u8, "
                       "src: &u8, n: usize);\n"
                       "fn main() {\n"
                       "  mut a := 1u8\n"
                       "  b := 2u8\n"
                       "  unsafe { memcopy(&mut a, &b, 1) }\n"
                       "  _ := a\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects an unsafe call in safe code") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe fn danger() {}\n"
                                      "fn main() {\n"
                                      "  danger()\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts an unsafe call inside an unsafe block") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe fn danger() {}\n"
                                      "fn main() {\n"
                                      "  unsafe { danger() }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check requires the gate in an unsafe function body") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe fn danger() {}\n"
                                      "unsafe fn wrap() {\n"
                                      "  danger()\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts the gate in an unsafe function body") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe fn danger() {}\n"
                                      "unsafe fn wrap() {\n"
                                      "  unsafe { danger() }\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check gates a generic unsafe call") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe fn pick<T>(x: T) -> T {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  ret pick::<i32>(1i32)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a gated intrinsic without the gate") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe intrinsic fn memcopy(dst: &mut "
                                      "u8, src: &u8, n: usize);\n"
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects an intrinsic missing its unsafe marker") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn memcopy(dst: &mut "
                                      "u8, src: &u8, n: usize);\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects an unsafe marker on a safe intrinsic") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe intrinsic fn size_of<T>() -> "
                                      "usize;\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts raw pointer reads and writes behind the gate") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  mut x := 1\n"
                                      "  p := &x as *i32\n"
                                      "  mut q := &mut x as *mut i32\n"
                                      "  unsafe { *q = 5 }\n"
                                      "  ret unsafe { *p }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects a raw dereference outside the gate") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  p := 0 as *i32\n"
                                      "  _ := *p\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a raw write through a shared pointer") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  mut p := 0 as *i32\n"
                                      "  *p = 1\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects casts that forge access") {
  VirtualDir shared;
  const bool setup_shared = write_all(shared, {{"main.al",
                                                "fn main() {\n"
                                                "  mut x := 1\n"
                                                "  _ := &x as *mut i32\n"
                                                "}\n"}});
  CHECK(setup_shared);
  if (!setup_shared) {
    return;
  }
  Fixture a;
  const CheckOutcome forged = check_case(shared, "main.al", {"main.al"}, a);
  CHECK(!forged.package.has_value());
  CHECK(a.bag.has_errors());

  VirtualDir to_reference;
  const bool setup_reference = write_all(to_reference, {{"main.al",
                                                         "fn main() {\n"
                                                         "  p := 0 as *i32\n"
                                                         "  _ := p as &i32\n"
                                                         "}\n"}});
  CHECK(setup_reference);
  if (!setup_reference) {
    return;
  }
  Fixture b;
  const CheckOutcome recovered =
      check_case(to_reference, "main.al", {"main.al"}, b);
  CHECK(!recovered.package.has_value());
  CHECK(b.bag.has_errors());
}

TEST_CASE("Check infers a raw offset's element type") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "unsafe intrinsic fn ptr_offset<T>(ptr: *T, "
                       "count: isize) -> *T;\n"
                       "fn main() -> i32 {\n"
                       "  mut a := [1, 2]\n"
                       "  p := &mut a[0] as *mut i32\n"
                       "  q := unsafe { ptr_offset(p, 1) }\n"
                       "  ret unsafe { *q }\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check gates an extern C call") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "extern \"C\" {\n"
                                      "  fn abs(x: i32) -> i32;\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  ret abs(-42)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts an extern C call behind the gate") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "extern \"C\" {\n"
                                      "  fn abs(x: i32) -> i32;\n"
                                      "  fn exit(code: i32);\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  ret unsafe { abs(-42) }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects a type that cannot cross a C boundary") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Pair { a: i32, b: i32 }\n"
                                      "extern \"C\" {\n"
                                      "  fn takes(p: Pair);\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a generic extern function") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "extern \"C\" {\n"
                                      "  fn pick<T>(x: T) -> T;\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects an unsafe function as a value") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe fn danger() {}\n"
                                      "fn main() {\n"
                                      "  _ := danger\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects unsafe methods") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct S { n: i32 }\n"
                                      "impl S {\n"
                                      "  unsafe fn poke(self: &Self) {}\n"
                                      "}\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects unknown intrinsics") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn frobnicate(x: i32);\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects mistyped intrinsic signatures") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "unsafe intrinsic fn memcopy(x: i32);\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects comp parameters on intrinsics") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "unsafe intrinsic fn memcopy(dst: &mut u8, "
                       "src: &u8, comp n: usize);\n"
                       "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves prelude calls without imports") {
  VirtualDir dir;
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
  const CheckOutcome result =
      check_case(dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64,
                 {{"core", "core.al"}});
  CHECK(result.package.has_value());
}

TEST_CASE("Check accepts string intrinsic declarations") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects mistyped string intrinsic signatures") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "intrinsic fn str_len(s: str) -> i32;\n"
                                      "fn main() {}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts array construction and indexing") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects heterogeneous array literals") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := [1i32, true]\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects oversized array repeats") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  _ := [0u8; 9999999999]\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects fmt arity mismatches") {
  VirtualDir dir;
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
  const CheckOutcome result =
      check_case(dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64,
                 {{"core", "core.al"}});
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects non-tuple fmt arguments") {
  VirtualDir dir;
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
  const CheckOutcome result =
      check_case(dir, "main.al", {"main.al"}, f, ir::PointerWidth::W64,
                 {{"core", "core.al"}});
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts generic free functions and turbofish arguments") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects uninferable generic call arguments") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts typed heap intrinsics") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub unsafe intrinsic fn alloc<T>(count: "
                       "usize) -> &mut MaybeUninit<T>;\n"
                       "pub unsafe intrinsic fn dealloc<T>(ptr: &mut "
                       "MaybeUninit<T>, count: usize);\n"
                       "pub intrinsic fn size_of<T>() -> "
                       "usize;\n"
                       "pub intrinsic fn align_of<T>() -> "
                       "usize;\n"
                       "pub unsafe intrinsic fn elem_ptr<T>(ptr: &mut "
                       "MaybeUninit<T>, index: usize) -> &mut "
                       "MaybeUninit<T>;\n"
                       "pub intrinsic fn uninit_write<T>(slot: "
                       "&mut MaybeUninit<T>, value: T);\n"
                       "pub unsafe intrinsic fn uninit_assume<T>(slot: "
                       "&mut MaybeUninit<T>) -> &mut T;\n"
                       "fn main() {\n"
                       "  data := unsafe { alloc::<i32>(4) }\n"
                       "  unsafe { uninit_write(elem_ptr(data, 0), 1i32) }\n"
                       "  _ := unsafe { *uninit_assume(elem_ptr(data, 0)) }\n"
                       "  _ := size_of::<i32>()\n"
                       "  _ := align_of::<i32>()\n"
                       "  unsafe { dealloc(data, 4) }\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE("Check rejects reading an uninitialized slot") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub unsafe intrinsic fn alloc<T>(count: "
                       "usize) -> &mut MaybeUninit<T>;\n"
                       "pub unsafe intrinsic fn elem_ptr<T>(ptr: &mut "
                       "MaybeUninit<T>, index: usize) -> &mut "
                       "MaybeUninit<T>;\n"
                       "fn main() -> i32 {\n"
                       "  data := unsafe { alloc::<i32>(1) }\n"
                       "  ret unsafe { *elem_ptr(data, 0) }\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects declaring MaybeUninit") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects an intrinsic declared with the wrong shape") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub unsafe intrinsic fn elem_ptr<T>(ptr: "
                       "&MaybeUninit<T>, index: usize) -> "
                       "&MaybeUninit<T>;\n"
                       "fn main() {\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects dereferencing a non-reference") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects assignment through a shared reference") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check resolves an associated function of a generic type") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
}

TEST_CASE(
    "Check rejects an associated function of a generic type without "
    "type arguments") {
  VirtualDir dir;
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
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

namespace {

// The type a struct local ends up with, found by its first field: a
// struct's field range holds storage copies so it stays contiguous, so
// this is not the index the field's own declaration published.
ir::TypeIdx struct_with_field(const CheckedPackage& package,
                              std::string_view field) {
  for (const CheckedModule& checked : package.modules) {
    for (const CheckedModule::StructInfo& info : checked.structs) {
      if (!info.fields.empty() && info.fields[0] == field) {
        return info.type;
      }
    }
  }
  return ir::TypeIdx(base::INVALID_IDX);
}

}  // namespace

TEST_CASE("Analyze marks a type with a destructor as needing one") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub unsafe intrinsic fn alloc<T>(count: usize) -> &mut "
                       "MaybeUninit<T>;\n"
                       "pub unsafe intrinsic fn dealloc<T>(ptr: &mut "
                       "MaybeUninit<T>, count: usize);\n"
                       "struct R { buf: &mut MaybeUninit<u8> }\n"
                       "impl R {\n"
                       "  fn drop(self: R) {\n"
                       "    unsafe { dealloc(self.buf, 1 as usize) }\n"
                       "  }\n"
                       "}\n"
                       "struct P { n: i32 }\n"
                       "fn main() {\n"
                       "  r := R { buf: unsafe { alloc::<u8>(4) } }\n"
                       "  p := P { n: 1 }\n"
                       "  _ := r.buf\n"
                       "  _ := p.n\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const ir::TypeIdx owns = struct_with_field(*result.package, "buf");
  const ir::TypeIdx plain = struct_with_field(*result.package, "n");
  CHECK(owns.is_valid());
  CHECK(plain.is_valid());
  if (!owns.is_valid() || !plain.is_valid()) {
    return;
  }
  CHECK(result.package->needs_drop[owns.idx]);
  CHECK(result.package->drop_glue[owns.idx].index != base::INVALID_IDX);
  CHECK(!result.package->needs_drop[plain.idx]);
  CHECK(result.package->drop_glue[plain.idx].index == base::INVALID_IDX);
}

TEST_CASE("Analyze propagates a destructor through a containing struct") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "pub unsafe intrinsic fn alloc<T>(count: usize) -> &mut "
                       "MaybeUninit<T>;\n"
                       "pub unsafe intrinsic fn dealloc<T>(ptr: &mut "
                       "MaybeUninit<T>, count: usize);\n"
                       "struct R { buf: &mut MaybeUninit<u8> }\n"
                       "impl R {\n"
                       "  fn drop(self: R) {\n"
                       "    unsafe { dealloc(self.buf, 1 as usize) }\n"
                       "  }\n"
                       "}\n"
                       "struct H { inner: R, tag: i32 }\n"
                       "fn main() {\n"
                       "  h := H { inner: R { buf: unsafe { alloc::<u8>(4) } },"
                       " tag: 1 }\n"
                       "  _ := h.tag\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const ir::TypeIdx holder = struct_with_field(*result.package, "inner");
  CHECK(holder.is_valid());
  if (!holder.is_valid()) {
    return;
  }
  // Ending it ends what it holds, even though it declares no destructor.
  CHECK(result.package->needs_drop[holder.idx]);
  CHECK(result.package->drop_glue[holder.idx].index == base::INVALID_IDX);
}

TEST_CASE("Analyze resolves a generic type's destructor") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"main.al",
             "pub unsafe intrinsic fn alloc<T>(count: usize) -> &mut "
             "MaybeUninit<T>;\n"
             "pub unsafe intrinsic fn dealloc<T>(ptr: &mut "
             "MaybeUninit<T>, count: usize);\n"
             "struct Box<T> { item: T,"
             " raw: &mut MaybeUninit<u8> }\n"
             "impl<T> Box<T> {\n"
             "  fn wrap(v: T) -> Box<T> {\n"
             "    ret Box { item: v, raw: unsafe { alloc::<u8>(1) } }\n"
             "  }\n"
             "  fn drop(self: Box<T>) {\n"
             "    unsafe { dealloc(self.raw, 1 as usize) }\n"
             "  }\n"
             "}\n"
             "fn main() -> i32 {\n"
             "  b := Box::<i32>::wrap(1i32)\n"
             "  ret b.item\n"
             "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  if (!result.package.has_value()) {
    return;
  }
  const ir::TypeIdx box = struct_with_field(*result.package, "item");
  CHECK(box.is_valid());
  if (!box.is_valid()) {
    return;
  }
  // The declared `Box<T>` and the `Box<i32>` it was instantiated on both
  // resolve a destructor: the instantiated one is what scope exit calls.
  CHECK(result.package->needs_drop[box.idx]);
  CHECK(result.package->drop_glue[box.idx].index != base::INVALID_IDX);
  for (ir::TypeIdx inst : result.package->generic_insts) {
    if (inst.idx >= result.package->needs_drop.size()) {
      continue;
    }
    CHECK(result.package->needs_drop[inst.idx]);
  }
}

TEST_CASE("Check reads a base prefix as digits, not a suffix") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  _ := 0xFF\n"
                                      "  _ := 0x1A\n"
                                      "  _ := 0xdeadbeef\n"
                                      "  _ := 0b1010\n"
                                      "  _ := 0o17\n"
                                      "  _ := 0xFFu8\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check rejects an unknown suffix after a base prefix") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  _ := 0xFFi\n"
                                      "  _ := 42i128\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a remainder or bitwise operator on a float") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> f64 {\n"
                                      "  ret 5.0 % 2.0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check calls closures and coerced functions") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"main.al",
             "fn inc(x: i32) -> i32 {\n"
             "  ret x + 1\n"
             "}\n"
             "fn apply(f: (i32) -> i32, x: i32) -> i32 {\n"
             "  ret f(x) + 1\n"
             "}\n"
             "fn main() -> i32 {\n"
             "  g := (a: i32) -> a + 1\n"
             "  h := ((v: i32) -> v * 2)(3)\n"
             "  ret apply(g, 1) + apply(inc, 2) + apply((n) -> n, h)\n"
             "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check infers closure parameters from the expected type") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn apply(f: (i32) -> i32, x: i32) -> i32 {\n"
                       "  ret f(x)\n"
                       "}\n"
                       "fn main() -> i32 {\n"
                       "  ret apply((a) -> a + 1, 1)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check rejects a closure arity mismatch") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn apply(f: (i32) -> i32, x: i32) -> i32 {\n"
                       "  ret f(x)\n"
                       "}\n"
                       "fn main() -> i32 {\n"
                       "  ret apply((a: i32, b: i32) -> a, 1)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects an unannotated parameter without context") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  g := (a) -> a + 1\n"
                                      "  ret g(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check accepts declared captures") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"main.al",
             "fn main() -> i32 {\n"
             "  mut t := 5\n"
             "  u := 1\n"
             "  mut v := 2\n"
             "  f := [t, &u, &mut v] (a: i32) -> { v = v + a; a + t + u }\n"
             "  ret f(1) + v\n"
             "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check reports a capture the body does not use") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  t := 5\n"
                                      "  f := [&t] (a: i32) -> a\n"
                                      "  ret f(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check reports a name the body uses but does not capture") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  t := 5\n"
                                      "  f := (a: i32) -> a + t\n"
                                      "  ret f(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check reports a capture no enclosing scope declares") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  f := [zz] (a: i32) -> a\n"
                                      "  ret f(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check reports a mutable capture of an immutable binding") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  t := 5\n"
                                      "  f := [&mut t] (a: i32) -> a\n"
                                      "  ret f(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check reports an inner capture the outer closure hides") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  t := 5\n"
                                      "  outer := [] (a: i32) -> {\n"
                                      "    inner := [&t] (b: i32) -> b + t\n"
                                      "    ret inner(a)\n"
                                      "  }\n"
                                      "  ret outer(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check reports a non-Copy move capture") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct R { x: i32, m: &mut i32 }\n"
                                      "fn take(r: R) -> i32 {\n"
                                      "  ret r.x\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  mut n := 0\n"
                                      "  r := R { x: 1, m: &mut n }\n"
                                      "  f := [r] () -> { ret take(r) }\n"
                                      "  ret f()\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Check rejects a function type as a generic argument") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn id<T>(x: T) -> T {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  f := id((a: i32) -> a)\n"
                                      "  ret f(1)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// A generic argument is refused in a type position too, not only when a
// generic call infers one: the mangle has no encoding for the signature,
// so two instantiations that differ there would share a symbol.
TEST_CASE("Check rejects a function type as a nominal type argument") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Box<T> {\n"
                                      "  value: T,\n"
                                      "}\n"
                                      "fn f(x: i32) -> i32 {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn take(b: Box<(i32) -> i32>) -> i32 {\n"
                                      "  ret 0\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  ret take(Box { value: f })\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// Two function types differ in their signature, not merely in where they
// were written; a value of one is not a value of the other.
TEST_CASE("Check rejects a function value with a different signature") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn apply(f: (i32) -> i32, x: i32) -> i32 {\n"
                       "  ret f(x)\n"
                       "}\n"
                       "fn inc64(x: i64) -> i64 {\n"
                       "  ret x + 1\n"
                       "}\n"
                       "fn main() -> i32 {\n"
                       "  ret apply(inc64, 1)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// The backend compares integers, floats and addresses; a function value
// is none of those, and reaching the emitter with one used to abort.
TEST_CASE("Check rejects a comparison of function values") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f(x: i32) -> i32 {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn g(x: i32) -> i32 {\n"
                                      "  ret x\n"
                                      "}\n"
                                      "fn main() -> i32 {\n"
                                      "  if f == g {\n"
                                      "    ret 1\n"
                                      "  }\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// A one-element tuple value can be annotated: the comma in the type is
// what says tuple, so the type is spellable.
TEST_CASE("Check spells a one-element tuple type") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  t: (i32,) := (1,)\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

// A turbofish on a value path has no arity check of its own, so the
// instantiation is where an argument list that does not match the
// declaration must be refused rather than indexed.
TEST_CASE("Check rejects a turbofish with the wrong arity") {
  for (const std::string_view call :
       {"Pair::<i32>::empty()", "Pair::<i32, i64, u8>::empty()"}) {
    VirtualDir dir;
    const std::string source =
        "struct Pair<A, B> { a: A, b: B }\n"
        "impl<A, B> Pair<A, B> {\n"
        "  fn empty() -> i32 { ret 7 }\n"
        "}\n"
        "fn main() -> i32 {\n"
        "  ret " +
        std::string(call) + "\n}\n";
    const bool setup = write_all(dir, {{"main.al", source}});
    CHECK(setup);
    if (!setup) {
      return;
    }
    Fixture f;
    const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
    CHECK(!result.package.has_value());
    CHECK(f.bag.has_errors());
  }
}

// The field check owns the "no fields" diagnostic, so a write through a
// scalar place is reported instead of reaching an internal lowering
// error.
TEST_CASE("Check rejects a field assignment on a scalar") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  mut x := 5\n"
                                      "  x.y = 1\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// A comp block inside a comp block is checked in the inner scope; the
// outer re-walk must not report the inner binding as unknown.
TEST_CASE("Check accepts a nested comp block") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  _ := comp { comp { k := 1\n"
                                      "    k } }\n"
                                      "  ret 0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

// Initializing one field twice is a typo that silently kept the last
// store.
TEST_CASE("Check rejects a duplicated struct field initializer") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct P { x: i32, y: i32 }\n"
                                      "fn main() -> i32 {\n"
                                      "  p := P { x: 1, x: 2, y: 3 }\n"
                                      "  ret p.x\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// The backend has no power operation; refusing it at checking keeps the
// unsupported construct out of lowering.
TEST_CASE("Check rejects the power operator") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() -> i32 {\n"
                                      "  x := 2\n"
                                      "  ret x ** 3\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
}

// A generic struct literal nested in another generic literal takes its
// instantiation from a field's storage copy, so the expectation is
// followed back to the instantiation it was copied from.
TEST_CASE("Check infers a nested generic struct literal") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct Inner<T> { v: T }\n"
                       "struct Outer<T> { i: Inner<T> }\n"
                       "fn f(x: Outer<i32>) -> Outer<i32> {\n"
                       "  ret Outer { i: Inner { v: 1i32 } }\n"
                       "}\n"
                       "fn main() -> i32 {\n"
                       "  o := f(Outer { i: Inner { v: 2i32 } })\n"
                       "  ret o.i.v\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check keeps one name apart per module") {
  // Two modules declaring the same name is the case a lookup narrowed to a
  // module's own entries has to get right: a bucket that held both, or the
  // wrong one, resolves a type to the other module's declaration and then
  // reports a field or a constructor that is not there.
  VirtualDir dir;
  const bool setup =
      write_all(dir, {
                         {"main.al",
                          "fn take(x: a::Box) -> i32 { ret x.n }\n"
                          "fn take2(x: b::Box) -> bool { ret x.flag }\n"
                          "fn main() {\n"
                          "  p := a::Box { n: 1 }\n"
                          "  q := b::Box { flag: true }\n"
                          "  _ := take(p)\n"
                          "  _ := take2(q)\n"
                          "}\n"},
                         {"a.al", "pub struct Box { n: i32 }\n"},
                         {"b.al", "pub struct Box { flag: bool }\n"},
                     });
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result =
      check_case(dir, "main.al", {"main.al", "a.al", "b.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

// A package that declares more names than the shared table can hold is
// refused with a diagnostic rather than aborting in the interner.
TEST_CASE("Check reports a spent name table") {
  std::string source;
  for (u32 i = 0; i < 64; ++i) {
    source += "struct S" + std::to_string(i) + " {}\n";
  }
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al", source}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f{16};
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  bool reported = false;
  for (u32 i = 0; i < f.bag.size(); ++i) {
    const diag::Diagnostic* const diag = f.bag.at(i);
    if (diag != nullptr &&
        diag->code ==
            diag::Code{diag::Stage::Analyzer,
                       static_cast<u8>(DiagCode::NameTableExhausted)}) {
      reported = true;
    }
  }
  CHECK(reported);
}

// A spec its package seals to a suite is implementable from inside
// that package or its suite, and from nowhere else (ADR-0053). These
// drive the checker directly because a local dependency cannot yet
// import a suite sibling's spec.
TEST_CASE("Check refuses a sealed spec implementation outside its package") {
  VirtualDir dir;
  const bool setup = write_all(
      dir,
      {{"specs.al", "pub spec Sealed {\n  fn seal(self: &Self) -> i32;\n}\n"},
       {"main.al",
        "struct Tag {\n  n: i32,\n}\n"
        "impl Sealed for Tag {\n"
        "  fn seal(self: &Self) -> i32 {\n    ret self.n\n  }\n}\n"
        "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string_view seals[] = {"Sealed"};
  const PackagePolicy policies[] = {
      {"core", "alcy/std", seals},
      {"testpkg", "", {}},
  };
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {{"core", "specs.al"}}, policies, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Spec 'Sealed' is sealed by package 'core'; implementing it from "
        "outside needs the [spec] implement key, which is not accepted "
        "yet");
}

TEST_CASE("Check admits a sealed spec implementation from the same suite") {
  VirtualDir dir;
  const bool setup = write_all(
      dir,
      {{"specs.al", "pub spec Sealed {\n  fn seal(self: &Self) -> i32;\n}\n"},
       {"main.al",
        "struct Tag {\n  n: i32,\n}\n"
        "impl Sealed for Tag {\n"
        "  fn seal(self: &Self) -> i32 {\n    ret self.n\n  }\n}\n"
        "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string_view seals[] = {"Sealed"};
  const PackagePolicy policies[] = {
      {"core", "alcy/std", seals},
      {"testpkg", "alcy/std", {}},
  };
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {{"core", "specs.al"}}, policies, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check reports a seal that names no declared spec") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec Shown {\n  fn show(self: &Self) -> i32;\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  const std::string_view seals[] = {"Shown", "Missing"};
  const PackagePolicy policies[] = {
      {"testpkg", "", seals},
  };
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {}, policies, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Manifest seals spec 'Missing', which package 'testpkg' does not "
        "declare");
}

// ADR-0053: a super-spec is resolved after every declaration is in, so
// a forward reference works, and an implementation owes an
// implementation of the super for the same target.
TEST_CASE("Check requires a super-spec implementation for the same target") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec B: A {\n  fn b(self: &Self) -> i32;\n}\n"
                       "spec A {\n  fn a(self: &Self) -> i32;\n}\n"
                       "struct Tag {\n  n: i32,\n}\n"
                       "impl B for Tag {\n"
                       "  fn b(self: &Self) -> i32 {\n    ret self.n\n  }\n}\n"
                       "impl A for Tag {\n"
                       "  fn a(self: &Self) -> i32 {\n    ret self.n\n  }\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check reports an implementation missing its super-spec") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec A {\n  fn a(self: &Self) -> i32;\n}\n"
                       "spec B: A {\n  fn b(self: &Self) -> i32;\n}\n"
                       "struct Tag {\n  n: i32,\n}\n"
                       "impl B for Tag {\n"
                       "  fn b(self: &Self) -> i32 {\n    ret self.n\n  }\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Implementing 'B' needs an implementation of 'A'");
}

TEST_CASE("Check matches a generic super-spec implementation by shape") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec A {\n  fn a(self: &Self) -> i32;\n}\n"
                       "spec B: A {\n  fn b(self: &Self) -> i32;\n}\n"
                       "struct Box<T> {\n  v: T,\n}\n"
                       "impl<T> B for Box<T> {\n"
                       "  fn b(self: &Self) -> i32 {\n    ret 0\n  }\n}\n"
                       "impl<U> A for Box<U> {\n"
                       "  fn a(self: &Self) -> i32 {\n    ret 0\n  }\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check reports a super-spec that is out of scope") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec B: Nope {\n  fn b(self: &Self) -> i32;\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message == "Super-spec 'Nope' must name a spec in scope");
}

TEST_CASE("Check reports a super-spec cycle") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec A: B {\n  fn a(self: &Self) -> i32;\n}\n"
                       "spec B: A {\n  fn b(self: &Self) -> i32;\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message == "Super-spec chain of 'A' forms a cycle");
}

TEST_CASE("Check reports an argument-bearing super-spec") {
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "spec A<T> {\n  fn a(self: &Self) -> i32;\n}\n"
                       "spec B: A<i32> {\n  fn b(self: &Self) -> i32;\n}\n"
                       "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Super-spec of 'B' takes arguments; generic supers are not supported "
        "yet");
}

TEST_CASE("Check binds a concrete spec argument in a generic impl") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"main.al",
             "spec P<I> {\n  fn p(self: &Self, i: I) -> i32;\n}\n"
             "struct Box<T> {\n  v: T,\n}\n"
             "impl<T> P<usize> for Box<T> {\n"
             "  fn p(self: &Self, i: usize) -> i32 {\n    ret 0\n  }\n}\n"
             "fn main() -> i32 {\n  b: Box<i32> := Box { v: 1 }\n  ret "
             "b.p(3)\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result = check_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

// ADR-0053: the operator specs are the compiler's, so a staged core
// declaration must keep the canonical shape; anything else is a
// different operator, not a library choice.
TEST_CASE("Check accepts a canonical core operator spec") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"specs.al",
             "pub spec Index<I, O> {\n  fn index(self: &Self, i: I) -> "
             "&O;\n}\n"},
            {"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {{"core", "specs.al"}}, {}, f);
  CHECK(result.package.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Check rejects a core operator spec off the canonical shape") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"specs.al",
             "pub spec Index<I, O> {\n  fn index(self: &Self, i: O) -> "
             "&O;\n}\n"},
            {"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {{"core", "specs.al"}}, {}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Spec 'Index' must keep the compiler's operator shape");
}

TEST_CASE("Check rejects a core equality spec off the canonical shape") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"specs.al",
             "pub spec PartialEq {\n  fn eq(self: &Self, other: &Self) -> "
             "i32;\n}\n"},
            {"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {{"core", "specs.al"}}, {}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Spec 'PartialEq' must keep the compiler's operator shape");
}

TEST_CASE("Check rejects a core index spec with a non-reference result") {
  VirtualDir dir;
  const bool setup = write_all(
      dir, {{"specs.al",
             "pub spec Index<I, O> {\n  fn index(self: &Self, i: I) -> "
             "O;\n}\n"},
            {"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const CheckOutcome result =
      check_with_policies(dir, "main.al", {{"core", "specs.al"}}, {}, f);
  CHECK(!result.package.has_value());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  CHECK(f.bag.at(0)->message ==
        "Spec 'Index' must keep the compiler's operator shape");
}

}  // namespace analyzer
