// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "tests/util/test_util.h"

#include <memory>

#include "fpag/debug/logger.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/logging/sync/sync_logger.h"
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
