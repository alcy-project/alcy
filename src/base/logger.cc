// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

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
