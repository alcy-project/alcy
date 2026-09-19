// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/driver_main.h"

#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/io/temp_dir.h"

namespace app {

namespace {

bool write_all(io::TempDir& dir, std::string_view rel, std::string_view text) {
  return dir.write_file(rel, text);
}

i32 run_check_on(io::TempDir& dir, std::string_view rel) {
  const std::string target = dir.join(rel);
  std::vector<std::string> storage{"alcy", "check", target};
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return driver_main(static_cast<i32>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("Check accepts a well-typed file") {
  io::TempDir dir("alcy_driver_check_ok_test");
  const bool setup = write_all(dir, "ok.al",
                               "struct Point { x: i32, y: i32 }\n"
                               "fn main() {\n"
                               "  p := Point { x: 1, y: 2 }\n"
                               "  _ := p.x + p.y\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  CHECK(run_check_on(dir, "ok.al") == 0);
}

TEST_CASE("Check rejects a mistyped file") {
  io::TempDir dir("alcy_driver_check_bad_test");
  const bool setup = write_all(dir, "bad.al",
                               "fn main() {\n"
                               "  x: u8 := 42i32\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  CHECK(run_check_on(dir, "bad.al") != 0);
}

TEST_CASE("Check rejects a non-exhaustive match") {
  io::TempDir dir("alcy_driver_check_match_test");
  const bool setup = write_all(dir, "bad.al",
                               "enum Choice { Yes, No(i32) }\n"
                               "fn f(c: Choice) -> i32 {\n"
                               "  ret match c {\n"
                               "    Yes => 1,\n"
                               "  }\n"
                               "}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  CHECK(run_check_on(dir, "bad.al") != 0);
}

}  // namespace app
