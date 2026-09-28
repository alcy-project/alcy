// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/logging/log_level.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/logging/sync/sync_logger.h"

namespace tests {

using TestLogger =
    logging::SyncLogger<logging::StdoutSink, logging::LogLevel::Debug>;

extern TestLogger logger;

void init_logger();

// True when `bytes` starts with the platform's relocatable-object
// magic: ELF on Linux, 64-bit Mach-O on Apple systems, COFF on
// Windows. A named output keeps its mode rather than its extension,
// so this is what tells an object from an executable wearing its name.
bool is_object_bytes(std::string_view bytes);

}  // namespace tests
