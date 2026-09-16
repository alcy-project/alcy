// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

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
