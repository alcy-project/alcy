// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "analyzer/types.h"

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
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

CheckCase check_case(io::TempDir& dir,
                     std::string_view root_rel,
                     std::initializer_list<std::string_view> rels,
                     Fixture& f,
                     ir::PointerWidth width = ir::PointerWidth::W64) {
  std::vector<source::FileId> ids;
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
    }
    ids.push_back(id);
  }
  diag::Fallible<ModuleTree> tree_result =
      resolve_modules(root, ids, "testpkg", f.sources, f.arena, f.bag);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt};
  }
  ModuleTree tree = std::move(tree_result).unwrap();
  CheckedPackage checked = check_package(tree, width, f.bag).unwrap();
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

}  // namespace

TEST_CASE("Check interns structs with named fields") {
  io::TempDir dir("alcy_types_struct_test");
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
  io::TempDir dir("alcy_types_enum_test");
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

TEST_CASE("Check resolves annotations and signatures") {
  io::TempDir dir("alcy_types_sig_test");
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

TEST_CASE("Check resolves cross-module types") {
  io::TempDir dir("alcy_types_cross_test");
  const bool setup = write_all(
      dir,
      {
          {"main.al",
           "mod a;\nuse a::Point;\nstruct Holder { p: Point, q: a::Other }\nfn "
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

TEST_CASE("Check instantiates blessed types with dedup") {
  io::TempDir dir("alcy_types_blessed_test");
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: Result<i32, bool>) -> Option<i32> {\n"
                       "  ret a\n"
                       "}\n"
                       "fn g(b: Result<i32, bool>) -> i32 {\n"
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
}

TEST_CASE("Check maps pointer widths for sized integers") {
  io::TempDir dir("alcy_types_width_test");
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
  io::TempDir dir("alcy_types_unknown_test");
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
  io::TempDir dir("alcy_types_recursive_test");
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
  io::TempDir dir("alcy_types_refcycle_test");
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

TEST_CASE("Check rejects duplicate and reserved definitions") {
  {
    io::TempDir dir("alcy_types_dup_test");
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
  {
    io::TempDir dir("alcy_types_reserved_test");
    const bool setup = write_all(dir, {{"main.al",
                                        "struct Result { x: i32 }\n"
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
}

TEST_CASE("Check rejects malformed generics") {
  {
    io::TempDir dir("alcy_types_arity_test");
    const bool setup = write_all(dir, {{"main.al",
                                        "fn f(x: Result<i32>) -> i32 {\n"
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
    io::TempDir dir("alcy_types_generic_test");
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
  io::TempDir dir("alcy_types_mutfield_test");
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

TEST_CASE("Check exposes blessed shapes in the registry") {
  io::TempDir dir("alcy_types_registry_test");
  const bool setup =
      write_all(dir, {{"main.al",
                       "fn f(a: Result<i32, bool>) -> Option<i32> {\n"
                       "  ret a\n"
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
  CHECK(result.package->blessed.size() == 2);
  if (result.package->blessed.size() != 2) {
    return;
  }
  const ir::Storage& types = result.package->types;
  for (const CheckedPackage::BlessedType& entry : result.package->blessed) {
    CHECK(types.types()[entry.type].tag == ir::TypeTag::Enum);
    const ir::EnumType& enum_type =
        types.enum_types()[types.types()[entry.type].as_enum()];
    CHECK(enum_type.variants.size() == 2);
    const ir::EnumVariantType& first =
        types.enum_variant_types()[enum_type.variants.head()];
    const ir::EnumVariantType& second =
        types.enum_variant_types()[ir::EnumVariantTypeIdx(
            enum_type.variants.head().idx + 1)];
    if (entry.is_result) {
      CHECK(entry.args.size() == 2);
      CHECK(first.fields.size() == 1);
      CHECK(second.fields.size() == 1);
      CHECK(types.types()[first.fields[0]].tag == ir::TypeTag::I32);
      CHECK(types.types()[second.fields[0]].tag == ir::TypeTag::I1);
    } else {
      CHECK(entry.args.size() == 1);
      CHECK(first.fields.size() == 1);
      CHECK(second.fields.empty());
      CHECK(types.types()[first.fields[0]].tag == ir::TypeTag::I32);
    }
  }
}

TEST_CASE("Check judges Copy structurally") {
  io::TempDir dir("alcy_types_copy_test");
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

}  // namespace analyzer
