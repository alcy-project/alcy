// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "borrow/borrow.h"

#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "fpag/str/string_interner.h"
#include "ir/type.h"
#include "lower/lower.h"
#include "source/source.h"

namespace borrow {

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

bool check_case(io::TempDir& dir,
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
    return false;
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  diag::Fallible<analyzer::CheckedPackage> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.bag);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  diag::Fallible<lower::LoweredPackage> lowered_result =
      lower::lower_package(std::move(checked_result).unwrap(),
                           ir::PointerWidth::W64, f.strings, f.bag);
  if (lowered_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  check_borrows(std::move(lowered_result).unwrap(), f.bag);
  return !f.bag.has_errors();
}

}  // namespace

TEST_CASE("Borrow accepts shared borrows") {
  io::TempDir dir("alcy_borrow_shared_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct Point { x: i32 }\n"
                                      "fn get(p: &Point) -> i32 {\n"
                                      "  ret p.x\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  pt := Point { x: 1 }\n"
                                      "  a := &pt\n"
                                      "  b := &pt\n"
                                      "  _ := get(a) + get(b)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow rejects exclusive conflicts") {
  io::TempDir dir("alcy_borrow_conflict_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  m := &mut x\n"
                                      "  n := &mut x\n"
                                      "  _ := m\n"
                                      "  _ := n\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow rejects mixed conflicts") {
  io::TempDir dir("alcy_borrow_mixed_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  r := &x\n"
                                      "  m := &mut x\n"
                                      "  _ := r\n"
                                      "  _ := m\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow expires at last use") {
  io::TempDir dir("alcy_borrow_expiry_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  m := &mut x\n"
                                      "  _ := m\n"
                                      "  n := &mut x\n"
                                      "  _ := n\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow rejects use after move") {
  io::TempDir dir("alcy_borrow_move_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  h := H { r: &mut x }\n"
                                      "  y := h\n"
                                      "  _ := y\n"
                                      "  _ := h\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow joins maybe-moves across branches") {
  io::TempDir dir("alcy_borrow_join_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  mut x := 0\n"
                                      "  h := H { r: &mut x }\n"
                                      "  mut c := true\n"
                                      "  if c {\n"
                                      "    g := h\n"
                                      "    c = false\n"
                                      "    _ := g\n"
                                      "  }\n"
                                      "  _ := h\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow keeps sibling branches independent") {
  io::TempDir dir("alcy_borrow_sibling_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  mut x := 0\n"
                                      "  h := H { r: &mut x }\n"
                                      "  mut c := true\n"
                                      "  if c {\n"
                                      "    g := h\n"
                                      "    _ := g\n"
                                      "  } else {\n"
                                      "    _ := h\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow catches loop-carried moves") {
  io::TempDir dir("alcy_borrow_loop_move_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  mut x := 0\n"
                                      "  h := H { r: &mut x }\n"
                                      "  mut i := 0\n"
                                      "  while i < 3 {\n"
                                      "    g := h\n"
                                      "    _ := g\n"
                                      "    i = i + 1\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow expires loans across loop iterations") {
  io::TempDir dir("alcy_borrow_loop_expiry_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  while x < 10 {\n"
                                      "    r := &mut x\n"
                                      "    _ := r\n"
                                      "    x = x + 1\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow rejects conflicting loop borrows") {
  io::TempDir dir("alcy_borrow_loop_conflict_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  m := &mut x\n"
                                      "  mut i := 0\n"
                                      "  while i < 3 {\n"
                                      "    i = i + 1\n"
                                      "    if i == 2 {\n"
                                      "      n := &mut x\n"
                                      "      _ := n\n"
                                      "    }\n"
                                      "  }\n"
                                      "  _ := m\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow rejects use after by-value match") {
  io::TempDir dir("alcy_borrow_match_move_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "enum E { A(&mut i32), B }\n"
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  v := E::A(&mut x)\n"
                                      "  r := match v {\n"
                                      "    E::A(y) => 1,\n"
                                      "    E::B => 0,\n"
                                      "  }\n"
                                      "  _ := v\n"
                                      "  _ := r\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow tracks loans spilled into aggregates") {
  io::TempDir dir("alcy_borrow_spill_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  r := &mut x\n"
                                      "  h := H { r: r }\n"
                                      "  m := &mut x\n"
                                      "  _ := m\n"
                                      "  _ := h\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow rejects moves under live loans") {
  io::TempDir dir("alcy_borrow_invalidate_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  h := H { r: &mut x }\n"
                                      "  r := &h\n"
                                      "  y := h\n"
                                      "  _ := y\n"
                                      "  _ := r\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow rejects escaping references") {
  io::TempDir dir("alcy_borrow_escape_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f() -> &i32 {\n"
                                      "  x := 1\n"
                                      "  ret &x\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow accepts parameter returns") {
  io::TempDir dir("alcy_borrow_param_test");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn id(p: &i32) -> &i32 {\n"
                                      "  ret p\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  _ := id(&x)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

}  // namespace borrow
