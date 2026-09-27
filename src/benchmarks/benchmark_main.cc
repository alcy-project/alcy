// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fpag/base/numeric.h"
#include "fpag/debug/signal_handler.h"
#include "fpag/debug/terminate_handler.h"
#include "fpag/term/console.h"

namespace {

void init() {
  term::register_console();
  debug::register_terminate_handler();
  debug::register_signal_handlers();
}

}  // namespace

i32 main(i32 /* argc */, char** /* argv */) {
  init();
  return 0;
}

