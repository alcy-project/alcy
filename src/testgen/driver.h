// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "testgen/tests.h"

namespace testgen {

// How a run executes the tests it selected.
enum class Execution : u8 {
  // One child process per test. A test that aborts takes its own process
  // with it, so the run reports the rest and names the one that died.
  MultiProcess,
  // Every test in the process the run started, in the order a run
  // selected them. A panic ends the run at the test that panicked, which
  // is the cost of not paying for a process per test.
  SingleThread,
};

// The index a multi-process driver answers with when its argument names
// no test it holds. The run never asks for one it did not write, so this
// is a disagreement between the two rather than a test that failed.
inline constexpr i32 UNKNOWN_TEST_EXIT = 100;

// The source of the module a test run enters through. Its `main` calls the
// tests the run wrote into it and does nothing else: no test output, no
// bookkeeping the caller could not do better.
//
// The driver reaches each test through the package's own modules - or, for
// a test of the module the driver joins, by the name it was declared with -
// so it compiles against the package and against the intrinsics it
// declares itself: no standard library, no dependency the package did not
// already have. Every path it writes starts at `package`, the tree the run
// builds, so one driver serves a package entered through its own target
// and a package whose modules are staged in a suite member's tree alike.
// The run passes the index of every test it selected and
// the driver runs exactly those: a multi-process driver takes the single
// index it was started for, and a single-thread driver walks the list and
// says on standard error which test it is about to run, because in one
// process there is no other way to know where a run stopped.
std::string driver_source(std::span<const TestCase> tests, Execution execution);

// Whether `name` is one the generated driver declares at its module's own
// level, so a module that joins the driver can refuse it instead of
// colliding with it as a duplicate definition: the intrinsics the driver
// spells out, anything under the `alcy_test_` prefix - which covers the
// items it names and the aliases it imports - and, for a single-thread
// driver, the write it performs to say which test it is about to run.
bool driver_item_name(std::string_view name, Execution execution);

// The items of a module that take a name the driver declares, in
// declaration order. A duplicate definition would point at the generated
// module, which names neither the item the reader wrote nor the reason it
// cannot stay, so the caller reports these instead.
std::vector<ast::Ident> driver_collisions(std::span<const ast::ItemIdx> module,
                                          Execution execution,
                                          ast::AstArena& ast);

}  // namespace testgen
