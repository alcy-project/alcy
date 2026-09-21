// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "lower/lower.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "fpag/str/string_interner.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "source/source.h"

namespace lower {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};
  source::SourceManager sources;
  str::StringInterner strings{mem::page_size()};

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

struct LowerCase {
  std::optional<LoweredPackage> lowered;
  bool ok;
};

LowerCase lower_case(io::TempDir& dir,
                     std::string_view root_rel,
                     std::initializer_list<std::string_view> rels,
                     Fixture& f) {
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
  diag::Fallible<analyzer::ModuleTree> tree_result = analyzer::resolve_modules(
      root, ids, "testpkg", f.sources, f.arena, f.bag);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  diag::Fallible<analyzer::CheckedPackage> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.bag);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::CheckedPackage checked = std::move(checked_result).unwrap();
  diag::Fallible<LoweredPackage> lowered_result = lower_package(
      std::move(checked), ir::PointerWidth::W64, f.strings, f.bag);
  if (lowered_result.is_err()) {
    return {std::nullopt, false};
  }
  return {std::move(lowered_result).unwrap(), !f.bag.has_errors()};
}

}  // namespace

TEST_CASE("Lower straight-line arithmetic") {
  io::TempDir dir("alcy_lower_arith_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn add(a: i32, b: i32) -> i32 {\n"
                                      "  ret a + b * 2\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := add(1, 2)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(result.lowered->storage.functions().size() == 2);
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower structs tuples fields and borrows") {
  io::TempDir dir("alcy_lower_aggregate_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Point { x: i32, y: i32 }\n"
                                      "fn get(p: &Point) -> i32 {\n"
                                      "  ret p.x + p.y\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  p := Point { x: 1, y: 2 }\n"
                                      "  t := (p.x, true)\n"
                                      "  _ := get(&p) + t.0\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower lowers control flow to verifiable blocks") {
  io::TempDir dir("alcy_lower_control_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f(b: bool) -> i32 {\n"
                                      "  r := match b {\n"
                                      "    true => 1,\n"
                                      "    false => 0,\n"
                                      "  }\n"
                                      "  mut i := 0\n"
                                      "  while i < r {\n"
                                      "    i = i + 1\n"
                                      "  }\n"
                                      "  loop {\n"
                                      "    if i <= 0 {\n"
                                      "      break\n"
                                      "    }\n"
                                      "    i = i - 1\n"
                                      "  }\n"
                                      "  ret i\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower lowers enums matches and question propagation") {
  io::TempDir dir("alcy_lower_enum_test");
  const bool setup =
      write_all(dir, {{"main.al",
                       "enum Shape { Circle(i32), Rect }\n"
                       "fn area(s: Shape) -> i32 {\n"
                       "  r := match s {\n"
                       "    Shape::Circle(x) => x,\n"
                       "    Shape::Rect => 0,\n"
                       "  }\n"
                       "  ret r\n"
                       "}\n"
                       "fn calc(o: Option<i32>) -> Option<i32> {\n"
                       "  v := o?\n"
                       "  ret Some(v + 1)\n"
                       "}\n"
                       "fn main() {\n"
                       "  a := area(Shape::Circle(3))\n"
                       "  o: Option<i32> := Some(7)\n"
                       "  y := o.unwrap()\n"
                       "  _ := a\n"
                       "  _ := y\n"
                       "  _ := calc(o)\n"
                       "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  CHECK(ir::verify_storage(result.lowered->storage).is_ok());
}

TEST_CASE("Lower emits verifiable LLVM IR") {
  io::TempDir dir("alcy_lower_emit_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn add(a: i32, b: i32) -> i32 {\n"
                                      "  ret a + b\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  _ := add(40, 2)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  LowerCase result = lower_case(dir, "main.al", {"main.al"}, f);
  CHECK(result.ok);
  CHECK(result.lowered.has_value());
  if (!result.ok || !result.lowered.has_value()) {
    return;
  }
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module =
      std::make_unique<llvm::Module>("lower_emit_test", context);
  codegen_llvm::LlvmIrEmitter emitter(
      module.get(), std::move(result.lowered->storage), &f.strings);
  std::move(emitter).emit();
  CHECK(!llvm::verifyModule(*module));
}

}  // namespace lower
