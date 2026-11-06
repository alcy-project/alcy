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
  // The identifier the entry writes after `test`.
  std::string name;
  // What a run prints for this test: `<module>::<name>`.
  std::string label;
  // Whether the test sits in the module the run enters through. The driver
  // is that module's own code, so it calls such a test by its bare name and
  // imports nothing.
  bool in_root = false;
};

// Why a block item got no function. Each reason has its own diagnostic,
// which is why one list of refusals is enough to report them all.
enum class Refusal : u8 {
  // The entry writes a return type. A test returns unit, so a value has no
  // reader.
  Returns,
  // The name the run writes itself: the entry point of the module it
  // enters through.
  NamedMain,
  // A name the module declares at its own level. Both are items of one
  // namespace, so the module would hold the name twice.
  Collides,
  // A name another block item already took, with the same consequence.
  Duplicates,
};

// A block item a splice left out, and why.
struct RefusedItem {
  Refusal reason = Refusal::Returns;
  ast::Ident name;
};

// The name an item declares in its module's namespace, when the item is
// one that names itself: the kinds a splice's items compete with. The
// caller reports it, so it carries its span too.
std::optional<ast::Ident> declared_ident(const ast::ItemNode& item);

// What a test block contributes once it is a function.
struct SplicedModule {
  // The module's items, with one free function per spliced test appended
  // in declaration order. A module that declares no test gets its own
  // list back.
  std::span<const ast::ItemIdx> items;
  // The tests that got a function, in declaration order.
  std::vector<TestCase> tests;
  // The block items that got none, in declaration order.
  std::vector<RefusedItem> refused;
};

// Turns every `test` block in `items` into free functions, which is the
// only form in which a test is called: the block itself is a declaration
// of intent, and a function is what runs.
//
// The entries keep their bodies as written - skipping the copy keeps the
// desugared tree the one that runs - and gain `pub`, because a run calls
// them from another module of the same package. The block stays in the
// list: the tree keeps saying what the source said.
//
// An entry or helper joins the module's namespace, so the ones that cannot
// take a free name there are left out instead of spliced, and `refused`
// names them and the reason: a reader wants the name a test clashed with
// rather than a duplicate-definition notice from resolution. The caller
// reports them, because a diagnostic carries a message and a source span
// that belong with the command's other input reports.
//
// Every node and list is allocated in `ast`, whose lifetime outlives the
// module's items.
SplicedModule splice_tests(std::span<const ast::ItemIdx> items,
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
