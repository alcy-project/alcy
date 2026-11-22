// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"

namespace testgen {

// One test a package declared, named the way a run reports it.
struct TestCase {
  // The module path inside the package, spelled as [modules] spells it:
  // `text`, or `collections/vector` for a module in a subdirectory.
  // Never empty, because a run's entry module is the driver.
  std::string module;
  // The identifier of the `#[test] fn` that declares it.
  std::string name;
  // What a run prints for this test: `<module>::<name>`.
  std::string label;
  // Whether the test sits in the module the run enters through. The driver
  // is that module's own code, so it calls such a test by its bare name and
  // imports nothing.
  bool in_root = false;
};

// Why a `#[test] fn` got no call in the driver. Each reason has its own
// diagnostic, which is why one list of refusals is enough to report them all.
enum class Refusal : u8 {
  // The function writes a return type. A test returns unit, so a value
  // has no reader.
  Returns,
  // The name the run writes itself: the entry point of the module.
  NamedMain,
  // A name the module declares outside its tests: one namespace, so the
  // module would hold the name twice.
  Collides,
  // A name another test already took, with the same consequence.
  Duplicates,
};

// A test a gather left out, and why.
struct RefusedTest {
  Refusal reason = Refusal::Returns;
  ast::Ident name;
};

// The name an item declares in its module's namespace, when the item is
// one that names itself: the kinds a gathered test competes with. The
// caller reports it, so it carries its span too.
std::optional<ast::Ident> declared_ident(const ast::ItemNode& item);

// What a module's tests contribute once they are callable.
struct GatheredModule {
  // The module's items, with every gathered test replaced by a public
  // copy of its function. A module that declares no test gets its own
  // list back.
  std::span<const ast::ItemIdx> items;
  // The tests that got a call in the driver, in declaration order.
  std::vector<TestCase> tests;
  // The tests that got none, in declaration order.
  std::vector<RefusedTest> refused;
};

// Gathers the `#[test] fn`s in `items` and makes them public, which is
// the form the driver calls. The functions keep their bodies as written;
// skipping the copy keeps the desugared tree the one that runs.
//
// A test shares its module's namespace with the other items, so one that
// cannot take a free name there is left out and `refused` names it and
// the reason: a reader wants the name a test clashed with rather than a
// duplicate-definition notice from resolution. The caller reports them.
//
// Every node and list is allocated in `ast`, whose lifetime outlives the
// module's items.
GatheredModule gather_tests(std::span<const ast::ItemIdx> items,
                            std::string_view module,
                            ast::AstArena& ast);

// The items of the module a run enters through: the module's own, without
// its entry point, followed by the driver's. A run has one entry point and
// one module to declare it in, and a test body keeps seeing the module it
// was written in, which is where a package's other modules are reachable
// by the names the module's code already uses.
std::span<const ast::ItemIdx> entry_items(std::span<const ast::ItemIdx> module,
                                          std::span<const ast::ItemIdx> driver,
                                          ast::AstArena& ast);

}  // namespace testgen
