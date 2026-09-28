// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

namespace tests {

// Wires up fpag's debug logger, which is what reports a failed internal
// check. Test output goes through doctest and the cli, so the test
// binary has no logger of its own.
void init_logger();

// True when `bytes` starts with the platform's relocatable-object
// magic: ELF on Linux, 64-bit Mach-O on Apple systems, COFF on
// Windows. A named output keeps its mode rather than its extension,
// so this is what tells an object from an executable wearing its name.
bool is_object_bytes(std::string_view bytes);

}  // namespace tests
