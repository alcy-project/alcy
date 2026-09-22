// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

namespace io {
class TempDir;
}  // namespace io

namespace app {

// Writes the embedded runtime sources into dir for the system
// compiler. Returns false when either write fails.
bool stage_runtime(io::TempDir& dir);

// Staged file names, relative to the staging directory.
std::string_view runtime_header_name();
std::string_view runtime_source_name();

}  // namespace app
