// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/logging/log_level.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/logging/sync/sync_logger.h"
#include "fpag/term/color_style.h"

namespace base {

using Logger =
    logging::SyncLogger<logging::StdoutSink, logging::kDefaultLogLevel>;
extern Logger logger;

void init_logger(term::ColorStyle style);

}  // namespace base
