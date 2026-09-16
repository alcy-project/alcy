// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

// #include "benchmark/benchmark.h"
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

  // benchmark::MaybeReenterWithoutASLR(argc, argv);
  // char arg0_default[] = "benchmark";
  // char* args_default = reinterpret_cast<char*>(arg0_default);
  // if (!argv) {
  //   argc = 1;
  //   argv = &args_default;
  // }
  // benchmark::Initialize(&argc, argv);
  // if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
  //   return 1;
  // }
  // benchmark::RunSpecifiedBenchmarks();
  // benchmark::Shutdown();
  return 0;
}

