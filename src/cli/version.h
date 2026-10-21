// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

namespace cli {

// The version this binary was built from, as `--version` reports it. The
// value is commit-dependent and defined by one generated translation
// unit (`version_generated.cc`); the function keeps it out of every other
// file's compile command, so a new commit does not rebuild the cli.
std::string_view alcy_version();

}  // namespace cli
