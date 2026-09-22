// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tests/util/test_util.h"

#include "fpag/debug/logger.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/term/console.h"
// #include "fpag/mem/page_allocator.h"

namespace tests {

TestLogger logger;

void init_logger() {
  logger.init(logging::StdoutSink(
      nullptr, 0, term::console_color_style(term::Stream::Stdout), false));
  debug::init_debug_logger();
}

}  // namespace tests
