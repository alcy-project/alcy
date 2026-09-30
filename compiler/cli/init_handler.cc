// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/init_handler.h"

#include "fpag/debug/exit_handler.h"
#include "fpag/debug/signal_handler.h"
#include "fpag/debug/terminate_handler.h"
#include "fpag/term/console.h"

namespace cli {

void init_runtime() {
  term::register_console();
  debug::register_exit_handler();
  debug::register_terminate_handler();
  debug::register_signal_handlers();
}

}  // namespace cli
