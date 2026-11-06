// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/spawn.h"

#include <utility>
#include <vector>

#include "config/build_config.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace pipeline {

namespace {

TEST_CASE("Spawn rejects empty argv") {
  base::Result<SpawnOutcome, SpawnError> result = run_command({});
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == SpawnError::EmptyArgv);
  }
}

TEST_CASE("Spawn reports missing programs") {
  base::Result<SpawnOutcome, SpawnError> result =
      run_command({"alcy-no-such-program-xyz"});
  CHECK(result.is_err());
}

#if !BUILD_FLAG(IS_OS_ASMJS)
#if BUILD_FLAG(IS_OS_WIN)
TEST_CASE("Spawn forwards exit codes") {
  base::Result<SpawnOutcome, SpawnError> result =
      run_command({"cmd", "/c", "exit", "3"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    CHECK(std::move(result).unwrap().code == 3);
  }
}
#else
TEST_CASE("Spawn forwards exit codes") {
  base::Result<SpawnOutcome, SpawnError> result =
      run_command({"sh", "-c", "exit 3"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    CHECK(std::move(result).unwrap().code == 3);
  }
}
#endif
#endif

#if !BUILD_FLAG(IS_OS_ASMJS)
#if BUILD_FLAG(IS_OS_WIN)
TEST_CASE("Capture counts the child's standard output") {
  base::Result<CapturedSpawn, SpawnError> result =
      run_command_capture({"cmd", "/c", "echo hello"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    const CapturedSpawn captured = std::move(result).unwrap();
    CHECK(captured.outcome.code == 0);
    CHECK(captured.output_bytes > 0);
  }
}
#else
TEST_CASE("Capture counts the child's standard output") {
  base::Result<CapturedSpawn, SpawnError> result =
      run_command_capture({"sh", "-c", "printf hello"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    const CapturedSpawn captured = std::move(result).unwrap();
    CHECK(captured.outcome.code == 0);
    CHECK(captured.output_bytes == 5);
  }
}

TEST_CASE("Capture reports the child's exit status") {
  base::Result<CapturedSpawn, SpawnError> result =
      run_command_capture({"sh", "-c", "printf quiet; exit 3"});
  CHECK(result.is_ok());
  if (result.is_ok()) {
    const CapturedSpawn captured = std::move(result).unwrap();
    CHECK(captured.outcome.code == 3);
    CHECK(captured.output_bytes == 5);
  }
}
#endif
#endif

}  // namespace

}  // namespace pipeline
