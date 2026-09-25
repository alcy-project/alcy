// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "borrow/borrow.h"

#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_interner.h"
#include "ir/type.h"
#include "lower/lower.h"
#include "source/source.h"

namespace borrow {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
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
  std::vector<analyzer::ModuleInput> inputs;
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
  diag::Fallible<analyzer::ModuleTree> tree_result = analyzer::resolve_modules(
      root, inputs, "testpkg", f.sources, f.ast, f.bag);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  diag::Fallible<analyzer::CheckedPackage> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.ast, f.bag);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  diag::Fallible<lower::LoweredPackage> lowered_result =
      lower::lower_package(std::move(checked_result).unwrap(),
                           ir::PointerWidth::W64, f.ast, f.strings, f.bag);
  if (lowered_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  check_borrows(std::move(lowered_result).unwrap(), f.bag);
  return !f.bag.has_errors();
}

}  // namespace

TEST_CASE("Borrow accepts shared borrows") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_shared_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_conflict_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_mixed_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_expiry_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_move_test_");
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

TEST_CASE("Borrow rejects a read after a by-value call move") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_call_move_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn consume(h: H) -> i32 {\n"
                                      "  ret *h.r\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  x := 1\n"
                                      "  h := H { r: &mut x }\n"
                                      "  n := consume(h)\n"
                                      "  _ := n\n"
                                      "  _ := h.r\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow reads a moved value into the call that moved it") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_move_read_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { n: i32 }\n"
                                      "fn consume(h: H) -> i32 {\n"
                                      "  ret h.n\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  h := H { n: 1 }\n"
                                      "  if consume(h) != 1 {\n"
                                      "    panic(\"bad\")\n"
                                      "  }\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Borrow joins maybe-moves across branches") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_join_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_sibling_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_loop_move_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_loop_expiry_test_");
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
  io::TempDir dir =
      io::TempDir::create_unique("alcy_borrow_loop_conflict_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_match_move_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_spill_test_");
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

TEST_CASE("Borrow reifies only returned parameters") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_summary_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn fst(a: &i32, b: &i32) -> &i32 {\n"
                                      "  ret a\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  p := 1\n"
                                      "  q := 2\n"
                                      "  x := fst(&p, &q)\n"
                                      "  m := &mut q\n"
                                      "  _ := x\n"
                                      "  _ := m\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow summarizes struct returns") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_borrow_struct_summary_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn wrap(r: &mut i32, s: &i32) -> H {\n"
                                      "  ret H { r: r }\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  y := 2\n"
                                      "  h := wrap(&mut x, &y)\n"
                                      "  m := &mut y\n"
                                      "  _ := h\n"
                                      "  _ := m\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow rejects conflicts through summaries") {
  io::TempDir dir =
      io::TempDir::create_unique("alcy_borrow_summary_conflict_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "struct H { r: &mut i32 }\n"
                                      "fn proj(h: H) -> &mut i32 {\n"
                                      "  ret h.r\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  mut x := 1\n"
                                      "  h := H { r: &mut x }\n"
                                      "  r := proj(h)\n"
                                      "  n := &mut x\n"
                                      "  _ := n\n"
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

TEST_CASE("Borrow summarizes recursive functions") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_recursion_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn f(n: i32, x: &i32) -> &i32 {\n"
                                      "  if n <= 0 {\n"
                                      "    ret x\n"
                                      "  }\n"
                                      "  ret f(n - 1, x)\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  a := 1\n"
                                      "  r := f(3, &a)\n"
                                      "  _ := r\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f));
}

TEST_CASE("Borrow iterates summaries to a fixed point") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_fixpoint_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn a(n: i32, x: &i32) -> &i32 {\n"
                                      "  ret b(n, x)\n"
                                      "}\n"
                                      "fn b(n: i32, x: &i32) -> &i32 {\n"
                                      "  if n <= 0 {\n"
                                      "    ret x\n"
                                      "  }\n"
                                      "  ret a(n - 1, x)\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  p := 1\n"
                                      "  r := a(2, &p)\n"
                                      "  m := &mut p\n"
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

TEST_CASE("Borrow rejects assignment while borrowed") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_assign_test_");
  const bool setup = write_all(dir, {{"main.al",
                                      "fn main() {\n"
                                      "  mut a := 1\n"
                                      "  r := &mut a\n"
                                      "  a = 10\n"
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

TEST_CASE("Borrow rejects moves under live loans") {
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_invalidate_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_escape_test_");
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
  io::TempDir dir = io::TempDir::create_unique("alcy_borrow_param_test_");
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
