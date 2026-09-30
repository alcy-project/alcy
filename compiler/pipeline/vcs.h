// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

// The version control system a scaffolded package is prepared for. It
// selects which ignore file is written, nothing more: no repository is
// ever created, because initializing one is the user's decision to make
// with the tool they chose.
enum class Vcs : u8 {
  Git,
  None,
};

}  // namespace pipeline
