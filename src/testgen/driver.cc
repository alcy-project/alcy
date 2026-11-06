// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "testgen/driver.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "testgen/tests.h"

namespace testgen {

namespace {

// The name a test's function answers to in the driver: the index it has
// in the run, so that the driver and the caller agree on the order
// without either holding a table.
std::string alias_of(usize index) {
  return "alcy_test_" + std::to_string(index);
}

// The spelling a `use` path starts with when it names a module of the tree
// being built. The driver is that tree's root module when the package
// names no target, and stands inside it otherwise, so `package` names the
// same modules either way and the driver needs no name of its own.
constexpr std::string_view PACKAGE_PATH = "package";

// The identifiers the driver declares at its module's own level, shared by
// the emitters and by the reservation an entry module's items are checked
// against: an emitter that spells a new name without it being reserved
// here would let a module declare the same one, and the duplicate
// definition would point at the generated file. The `alcy_test_` prefix
// covers the driver's own items and the aliases it imports, so only these
// need spelling out.
constexpr std::string_view ARG_COUNT = "arg_count";
constexpr std::string_view ARG = "arg";
constexpr std::string_view STR_LEN = "str_len";
constexpr std::string_view STR_BYTE = "str_byte";
// The write a single-thread driver performs to say which test it is about
// to run; a multi-process driver never declares it.
constexpr std::string_view SYS_WRITE = "sys_write";

// A module path spelled as a `use` path: `collections/vector` is the
// module `vector` inside `collections`, reached from the tree's root.
std::string use_path(std::string_view module, std::string_view name) {
  std::string path(PACKAGE_PATH);
  path += "::";
  for (const char c : module) {
    path += c == '/' ? "::" : std::string(1, c);
  }
  path += "::";
  path += name;
  return path;
}

// What the driver writes to call one test: the alias it imported, or, for
// a test of the module the driver itself is part of, the name as written.
std::string callee_of(usize index, const TestCase& test) {
  return test.in_root ? test.name : alias_of(index);
}

void append_imports(std::string& out, std::span<const TestCase> tests) {
  bool imported = false;
  for (usize i = 0; i < tests.size(); ++i) {
    const TestCase& test = tests[i];
    if (test.in_root) {
      continue;
    }
    out += "use ";
    out += use_path(test.module, test.name);
    out += " as ";
    out += alias_of(i);
    out += ";\n";
    imported = true;
  }
  if (imported) {
    out += '\n';
  }
}

// The run passes each test's index in decimal, which is the only way an
// argument arrives: a text the driver has to read back one digit at a
// time, because there is no std here to parse it.
void append_argument_intrinsics(std::string& out) {
  out += "unsafe intrinsic fn ";
  out += ARG_COUNT;
  out += "() -> i64;\n";
  out += "unsafe intrinsic fn ";
  out += ARG;
  out += "(i: i64) -> str;\n";
  out += "intrinsic fn ";
  out += STR_LEN;
  out += "(s: str) -> usize;\n";
  out += "intrinsic fn ";
  out += STR_BYTE;
  out += "(s: str, i: usize) -> u8;\n\n";
  out += "fn alcy_test_index(text: str) -> i64 {\n";
  out += "  mut value := 0 as i64;\n";
  out += "  mut at := 0 as usize;\n";
  out += "  while at < ";
  out += STR_LEN;
  out += "(text) {\n";
  out += "    value = value * 10 + ((";
  out += STR_BYTE;
  out += "(text, at) as i64) - 48);\n";
  out += "    at = at + 1;\n";
  out += "  }\n";
  out += "  ret value\n";
  out += "}\n\n";
}

void append_note(std::string& out) {
  out += "intrinsic fn ";
  out += SYS_WRITE;
  out += "(fd: i32, buf: str) -> i64;\n\n";
  out += "fn alcy_test_note(text: str) {\n";
  out += "  _ := ";
  out += SYS_WRITE;
  out += "(2, text);\n";
  out += "}\n\n";
}

// One test per index, so the two executions differ in how many tests one
// process runs and not in how a test is called.
void append_dispatch(std::string& out,
                     std::span<const TestCase> tests,
                     Execution execution) {
  out += "fn alcy_test_run(index: i64) -> bool {\n";
  for (usize i = 0; i < tests.size(); ++i) {
    out += "  if index == ";
    out += std::to_string(i);
    out += " {\n";
    if (execution == Execution::SingleThread) {
      // The process carries several tests, so it is the one that can say
      // which of them is about to run. It says so on standard error, where
      // the person watching a run reads it: the run itself learns only that
      // the process ended, and the note is what names the test it ended in.
      // The label is a module path and an identifier, so it needs no escape
      // to sit in a string literal.
      out += "    alcy_test_note(\"alcy-test: running ";
      out += tests[i].label;
      out += "\\n\");\n";
    }
    out += "    ";
    out += callee_of(i, tests[i]);
    out += "();\n    ret true\n  }\n";
  }
  out += "  ret false\n}\n\n";
}

void append_multi_process_main(std::string& out) {
  out += "fn main() -> i32 {\n";
  out += "  if unsafe { ";
  out += ARG_COUNT;
  out += "() } < 2 {\n    ret ";
  out += std::to_string(UNKNOWN_TEST_EXIT);
  out += ";\n  }\n";
  out += "  if alcy_test_run(alcy_test_index(unsafe { ";
  out += ARG;
  out += "(1) })) {\n";
  out += "    ret 0;\n  }\n";
  out += "  ret ";
  out += std::to_string(UNKNOWN_TEST_EXIT);
  out += ";\n}\n";
}

void append_single_thread_main(std::string& out) {
  out += "fn main() -> i32 {\n";
  out += "  mut at := 1 as i64;\n";
  out += "  while at < unsafe { ";
  out += ARG_COUNT;
  out += "() } {\n";
  out += "    _ := alcy_test_run(alcy_test_index(unsafe { ";
  out += ARG;
  out += "(at) }));\n";
  out += "    at = at + 1;\n  }\n";
  out += "  ret 0;\n}\n";
}

}  // namespace

// The text this builds is the contract: the run parses and checks it as
// the module it enters through, and nothing between the appends above and
// that parse checks it, so a drift here surfaces as the run's own
// diagnostic for a module the reader never wrote. driver_test.cc pins the
// declared names against the reservation to keep the two from drifting.
std::string driver_source(std::span<const TestCase> tests,
                          Execution execution) {
  if (tests.empty()) {
    return "fn main() -> i32 {\n  ret 0;\n}\n";
  }
  std::string out;
  append_imports(out, tests);
  append_argument_intrinsics(out);
  if (execution == Execution::SingleThread) {
    append_note(out);
  }
  append_dispatch(out, tests, execution);
  if (execution == Execution::MultiProcess) {
    append_multi_process_main(out);
  } else {
    append_single_thread_main(out);
  }
  return out;
}

bool driver_item_name(std::string_view name, Execution execution) {
  constexpr std::string_view PREFIX = "alcy_test_";
  if (name.starts_with(PREFIX)) {
    return true;
  }
  if (name == ARG_COUNT || name == ARG || name == STR_LEN || name == STR_BYTE) {
    return true;
  }
  return execution == Execution::SingleThread && name == SYS_WRITE;
}

std::vector<ast::Ident> driver_collisions(std::span<const ast::ItemIdx> module,
                                          Execution execution,
                                          ast::AstArena& ast) {
  std::vector<ast::Ident> clashes;
  for (const ast::ItemIdx index : module) {
    if (!ast.items.bound(index)) {
      continue;
    }
    const std::optional<ast::Ident> ident = declared_ident(ast.items[index]);
    if (ident.has_value() && driver_item_name(ident->name, execution)) {
      clashes.push_back(*ident);
    }
  }
  return clashes;
}

}  // namespace testgen
