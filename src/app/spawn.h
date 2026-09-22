// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace app {

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

}  // namespace app
