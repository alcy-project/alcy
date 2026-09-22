// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/spawn.h"

#include <string>
#include <utility>
#include <vector>

#include "cfg/build_config.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace app {

namespace {

TEST_CASE("Spawn rejects empty argv") {
  base::Result<i32, SpawnError> result = run_command({});
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == SpawnError::EmptyArgv);
  }
}

TEST_CASE("Spawn reports missing programs") {
  base::Result<i32, SpawnError> result =
      run_command({"alcy-no-such-program-xyz"});
  CHECK(result.is_err());
}

#if !BUILD_FLAG(IS_OS_ASMJS)
#if BUILD_FLAG(IS_OS_WIN)
TEST_CASE("Spawn forwards exit codes") {
  base::Result<i32, SpawnError> result =
      run_command({"cmd", "/c", "exit", "3"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    CHECK(std::move(result).unwrap() == 3);
  }
}
#else
TEST_CASE("Spawn forwards exit codes") {
  base::Result<i32, SpawnError> result = run_command({"sh", "-c", "exit 3"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    CHECK(std::move(result).unwrap() == 3);
  }
}
#endif
#endif

}  // namespace

}  // namespace app
