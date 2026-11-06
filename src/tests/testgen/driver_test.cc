// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "testgen/driver.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "testgen/tests.h"

namespace {

testgen::TestCase root_test(std::string_view name) {
  return testgen::TestCase{.module = "main",
                           .name = std::string(name),
                           .label = "main::" + std::string(name),
                           .in_root = true};
}

testgen::TestCase package_test(std::string_view module, std::string_view name) {
  return testgen::TestCase{
      .module = std::string(module),
      .name = std::string(name),
      .label = std::string(module) + "::" + std::string(name),
      .in_root = false};
}

}  // namespace

TEST_CASE("A driver with no test still declares the entry point") {
  // A package of helpers and no test cannot be entered through a module of
  // its own, so the driver stands in for it and does nothing.
  CHECK(testgen::driver_source({}, testgen::Execution::MultiProcess) ==
        "fn main() -> i32 {\n  ret 0;\n}\n");
}

TEST_CASE("A multi-process driver calls one test by the index it was given") {
  const std::vector<testgen::TestCase> tests = {
      package_test("collections/vector", "push"),
      root_test("adds"),
  };
  const std::string source =
      testgen::driver_source(tests, testgen::Execution::MultiProcess);

  // A test of another module arrives through a path that starts at the
  // tree the run builds, under an alias the index names; a test of the
  // driver's own module is already in scope.
  CHECK(
      source.find("use package::collections::vector::push as alcy_test_0;\n") !=
      std::string::npos);
  CHECK(source.find("if index == 0 {\n    alcy_test_0();\n    ret true\n  }") !=
        std::string::npos);
  CHECK(source.find("if index == 1 {\n    adds();\n    ret true\n  }") !=
        std::string::npos);
  // The index arrives as text, because there is no standard library in the
  // tree the driver joins to parse it.
  CHECK(source.find("fn alcy_test_index(text: str) -> i64 {") !=
        std::string::npos);
  CHECK(source.find("if unsafe { arg_count() } < 2 {\n    ret ") !=
        std::string::npos);
  CHECK(source.find("ret " + std::to_string(testgen::UNKNOWN_TEST_EXIT)) !=
        std::string::npos);
  // One process carries one test, so nothing has to say which test it is.
  CHECK(source.find("alcy_test_note") == std::string::npos);
  CHECK(source.find("sys_write") == std::string::npos);
}

TEST_CASE("A single-thread driver says which test it is about to run") {
  const std::vector<testgen::TestCase> tests = {
      package_test("text", "parses"),
      root_test("adds"),
  };
  const std::string source =
      testgen::driver_source(tests, testgen::Execution::SingleThread);

  // The note sits inside the branch for the test it names, so a run that
  // stops in that test leaves the line on standard error: in one process
  // the exit status cannot say which test produced it.
  CHECK(source.find(
            "if index == 0 {\n"
            "    alcy_test_note(\"alcy-test: running text::parses\\n\");\n"
            "    alcy_test_0();\n    ret true\n  }") != std::string::npos);
  CHECK(
      source.find("if index == 1 {\n"
                  "    alcy_test_note(\"alcy-test: running main::adds\\n\");\n"
                  "    adds();\n    ret true\n  }") != std::string::npos);
  CHECK(source.find("fn alcy_test_note(text: str) {\n"
                    "  _ := sys_write(2, text);\n}") != std::string::npos);
  // The one process walks the indices the run passed it and enters none of
  // them by argument.
  CHECK(source.find("mut at := 1 as i64;") != std::string::npos);
  CHECK(source.find("while at < unsafe { arg_count() } {") !=
        std::string::npos);
}

TEST_CASE("A driver keeps the order of the tests it was written for") {
  const std::vector<testgen::TestCase> tests = {
      root_test("first"),
      root_test("second"),
  };
  const std::string source =
      testgen::driver_source(tests, testgen::Execution::MultiProcess);

  const std::string_view text(source);
  const usize first = text.find("first();");
  const usize second = text.find("second();");
  CHECK(first != std::string::npos);
  CHECK(second != std::string::npos);
  CHECK(first < second);
}

TEST_CASE("driver_item_name reserves the driver's own items") {
  // The intrinsics the driver spells out, its own items, and the aliases it
  // imports for other modules' tests.
  CHECK(testgen::driver_item_name("alcy_test_run",
                                  testgen::Execution::MultiProcess));
  CHECK(testgen::driver_item_name("alcy_test_index",
                                  testgen::Execution::MultiProcess));
  CHECK(testgen::driver_item_name("alcy_test_note",
                                  testgen::Execution::SingleThread));
  CHECK(testgen::driver_item_name("alcy_test_0",
                                  testgen::Execution::MultiProcess));
  CHECK(testgen::driver_item_name("alcy_test_",
                                  testgen::Execution::MultiProcess));
  CHECK(
      testgen::driver_item_name("arg_count", testgen::Execution::MultiProcess));
  CHECK(testgen::driver_item_name("arg", testgen::Execution::MultiProcess));
  CHECK(testgen::driver_item_name("str_len", testgen::Execution::MultiProcess));
  CHECK(
      testgen::driver_item_name("str_byte", testgen::Execution::MultiProcess));
  CHECK_FALSE(
      testgen::driver_item_name("main", testgen::Execution::MultiProcess));
  CHECK_FALSE(testgen::driver_item_name("alcy_testing",
                                        testgen::Execution::MultiProcess));
  CHECK_FALSE(testgen::driver_item_name("alcytest_run",
                                        testgen::Execution::MultiProcess));
  CHECK_FALSE(
      testgen::driver_item_name("helper", testgen::Execution::MultiProcess));
  // The write a single-thread driver performs to say which test it is
  // about to run belongs to that mode alone.
  CHECK(
      testgen::driver_item_name("sys_write", testgen::Execution::SingleThread));
  CHECK_FALSE(
      testgen::driver_item_name("sys_write", testgen::Execution::MultiProcess));
}

namespace {

// The module-level names a generated driver declares: the functions and
// intrinsics it defines, and the import aliases it takes. `main` is the
// entry point the run replaces rather than a name a module keeps, so the
// reservation does not cover it.
std::vector<std::string> declared_names(const std::string& source) {
  std::vector<std::string> names;
  for (usize at = 0; at < source.size();) {
    const usize newline = source.find('\n', at);
    const usize line_end =
        newline == std::string::npos ? source.size() : newline;
    const std::string_view line(source.data() + at, line_end - at);
    at = newline == std::string::npos ? source.size() : newline + 1;
    usize name_at = std::string_view::npos;
    usize name_end = std::string_view::npos;
    if (line.starts_with("fn ") || line.starts_with("intrinsic fn ") ||
        line.starts_with("unsafe intrinsic fn ")) {
      name_at = line.find("fn ") + 3;
      name_end = line.find('(', name_at);
    } else if (line.starts_with("use ")) {
      const usize as_at = line.find(" as ");
      if (as_at != std::string_view::npos) {
        name_at = as_at + 4;
        name_end = line.find(';', name_at);
      }
    }
    if (name_at == std::string_view::npos ||
        name_end == std::string_view::npos) {
      continue;
    }
    std::string name(line.substr(name_at, name_end - name_at));
    if (name == "main") {
      continue;
    }
    names.push_back(std::move(name));
  }
  return names;
}

}  // namespace

TEST_CASE("The reservation covers every name a generated driver declares") {
  const std::vector<testgen::TestCase> tests = {
      package_test("collections/vector", "push"),
      package_test("text", "parses"),
      root_test("adds"),
  };
  for (const testgen::Execution execution :
       {testgen::Execution::MultiProcess, testgen::Execution::SingleThread}) {
    const std::string source = testgen::driver_source(tests, execution);
    const std::vector<std::string> names = declared_names(source);
    CHECK(names.size() >= 7);
    for (const std::string& name : names) {
      CHECK_MESSAGE(testgen::driver_item_name(name, execution), name,
                    " is declared by the generated driver but the"
                    " reservation does not cover it");
    }
  }
}
