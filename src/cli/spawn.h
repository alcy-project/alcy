// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace cli {

enum class SpawnError : u8 {
  EmptyArgv,
  SpawnFailed,
  WaitFailed,
  BadExit,
};

// Runs argv synchronously without a shell, returning the process
// exit code. Arguments must not contain NUL bytes. A process killed
// by a signal reports BadExit.
base::Result<i32, SpawnError> run_command(const std::vector<std::string>& argv);

}  // namespace cli
