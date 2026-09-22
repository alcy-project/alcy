// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/logging/log_level.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/logging/sync/sync_logger.h"

namespace tests {

using TestLogger =
    logging::SyncLogger<logging::StdoutSink, logging::LogLevel::Debug>;

extern TestLogger logger;

void init_logger();

}  // namespace tests
