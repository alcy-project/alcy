// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace pipeline {

enum class SpawnError : u8 {
  EmptyArgv,
  SpawnFailed,
  WaitFailed,
};

// How a child process ended. A process a signal killed has no exit
// status of its own, and reporting both through one number spends a
// range on a distinction the caller cannot see, so they are two fields.
struct SpawnOutcome {
  // The status the process exited with, or the signal that ended it.
  i32 code = 0;
  // Whether `code` names a signal rather than an exit status. A host
  // without signals reports the status its process died with instead, so
  // this is false for it however the process ended.
  bool signaled = false;
};

// What a child process wrote to standard output, when a caller asked
// this process to drain and count it rather than inherit it.
struct CapturedSpawn {
  SpawnOutcome outcome;
  // The bytes the child wrote to standard output.
  u64 output_bytes = 0;
};

// Runs argv synchronously without a shell. Arguments must not contain NUL
// bytes.
base::Result<SpawnOutcome, SpawnError> run_command(
    const std::vector<std::string>& argv);

// Runs argv like run_command, except that the child's standard output is a
// pipe this process drains and counts instead of the terminal, so the bytes
// never land in a stream this process renders its own output on. Standard
// input and standard error are inherited either way. The bytes are not kept:
// the count is the whole of what the caller does with them.
base::Result<CapturedSpawn, SpawnError> run_command_capture(
    const std::vector<std::string>& argv);

}  // namespace pipeline
