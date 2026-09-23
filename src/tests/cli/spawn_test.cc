// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/spawn.h"

#include <utility>
#include <vector>

#include "config/build_config.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace cli {

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

}  // namespace cli
