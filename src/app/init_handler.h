// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace app {

// Registers console, exit, terminate, and signal handlers. Does not touch
// the logger: driver_main initializes it after argument parsing so the
// resolved --color mode applies from the first message.
void init_runtime();

}  // namespace app
