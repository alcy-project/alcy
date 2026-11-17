// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdio>

#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/debug/signal_handler.h"
#include "fpag/debug/terminate_handler.h"
#include "fpag/term/console.h"
#include "tests/util/test_util.h"

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

void init() {
  // Tests redirect stdout - a capture inside a test, a file on CI - and
  // a fully buffered stream carries pending doctest text across the
  // redirect: under wasm a report landed inside the next test's capture
  // file. Unbuffered keeps each report on the descriptor it was for.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  term::register_console();
  debug::register_terminate_handler();
  debug::register_signal_handlers();
  tests::init_logger();
}

void clean_up() {
  // noop
}

i32 main(i32 argc, char** argv) {
  init();

  doctest::Context context;

  context.applyCommandLine(argc, argv);

  i32 result = 0;
  {
    PROFILE_SCOPE("run all tests");
    result = context.run();
  }

  if (context.shouldExit()) {
    clean_up();
    return result;
  }

  clean_up();

  return result;
}

