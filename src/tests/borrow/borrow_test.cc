// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "borrow/borrow.h"

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_interner.h"
#include "i18n/language.h"
#include "ir/type.h"
#include "lower/lower.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace borrow {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  source::SourceManager sources;
  str::StringInterner strings{mem::page_size()};

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

bool check_case(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    Fixture& f,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude = {}) {
  std::vector<analyzer::ModuleInput> inputs;
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
  std::deque<std::string> prelude_storage;
  std::vector<analyzer::ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    prelude_storage.emplace_back(name);
    // A staged prelude source is a package facade, so its public
    // surface is in scope without a `use`.
    prelude_inputs.push_back({prelude_storage.back(),
                              f.sources.add_virtual(file->name, file->bytes),
                              true});
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree_result =
      analyzer::resolve_modules(root, inputs, "testpkg", f.sources, f.ast,
                                f.bag, prelude_inputs);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  base::Result<analyzer::CheckedPackage, diag::Reported> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.ast, f.bag);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  base::Result<lower::LoweredPackage, diag::Reported> lowered_result =
      lower::lower_package(std::move(checked_result).unwrap(),
                           ir::PointerWidth::W64, f.ast, f.strings, f.bag);
  if (lowered_result.is_err() || f.bag.has_errors()) {
    return false;
  }
  if (check_borrows(std::move(lowered_result).unwrap(), f.bag).is_err()) {
    return false;
  }
  return !f.bag.has_errors();
}

}  // namespace

TEST_CASE("Borrow accepts shared borrows") {
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
  const bool setup =
      write_all(dir, {{"main.al",
                       "struct H { n: i32 }\n"
                       "fn consume(h: H) -> i32 {\n"
                       "  ret h.n\n"
                       "}\n"
                       "fn main() {\n"
                       "  h := H { n: 1 }\n"
                       "  if consume(h) != 1 {\n"
                       "    panic(\"bad\")\n"
                       "  }\n"
                       "}\n"},
                      {"core.al", "pub intrinsic fn panic(msg: str) -> !;\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(check_case(dir, "main.al", {"main.al"}, f, {{"core", "core.al"}}));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Borrow rejects a write through a reborrow") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "fn g(mut b: &mut i32) -> i32 {\n"
                                      "  x := &*b\n"
                                      "  *b = 1\n"
                                      "  ret *x\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  mut n := 0\n"
                                      "  _ := g(&mut n)\n"
                                      "}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(!check_case(dir, "main.al", {"main.al"}, f));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Borrow accepts a read through a shared receiver") {
  VirtualDir dir;
  const bool setup = write_all(dir, {{"main.al",
                                      "struct S { n: i32 }\n"
                                      "impl S {\n"
                                      "  fn get(self: &Self) -> &i32 {\n"
                                      "    ret &self.n\n"
                                      "  }\n"
                                      "  fn bump(mut self: &mut Self) {\n"
                                      "    self.n = self.n + 1\n"
                                      "  }\n"
                                      "}\n"
                                      "fn read_only(s: &S) -> i32 {\n"
                                      "  r := s.get()\n"
                                      "  ret *r\n"
                                      "}\n"
                                      "fn main() {\n"
                                      "  x := S { n: 1 }\n"
                                      "  mut y := S { n: 1 }\n"
                                      "  _ := x.get()\n"
                                      "  _ := read_only(&x)\n"
                                      "  y.bump()\n"
                                      "  r := y.get()\n"
                                      "  _ := *r\n"
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
  VirtualDir dir;
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
