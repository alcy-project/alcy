// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/logger.h"

#include <utility>

#include "fpag/debug/logger.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/term/color_style.h"

namespace base {

Logger logger;

void init_logger(term::ColorStyle style) {
  logging::StdoutSink sink(nullptr, 0, style, false);
  logger.init(std::move(sink));
  debug::init_debug_logger();
}

}  // namespace base
