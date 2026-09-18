// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/init_handler.h"

#include "fpag/debug/exit_handler.h"
#include "fpag/debug/signal_handler.h"
#include "fpag/debug/terminate_handler.h"
#include "fpag/term/console.h"

namespace app {

void init_runtime() {
  term::register_console();
  debug::register_exit_handler();
  debug::register_terminate_handler();
  debug::register_signal_handlers();
}

}  // namespace app
