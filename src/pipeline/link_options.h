// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

namespace pipeline {

// What the link step is handed: the driver to run, and the arguments that
// driver gets. Views borrow the caller's storage — argv for a flag, the
// goal package's toolchain.toml for a file — so one must not outlive
// them.
struct LinkOptions {
  // Empty selects the default toolchain driver.
  std::string_view driver;
  // Appended to the link command line after the objects, so they are the
  // driver's own arguments: a library, a search path, or a switch such as
  // -fuse-ld=lld. A flag meant for the linker behind the driver goes
  // through -Wl,.
  std::span<const std::string_view> args;
};

}  // namespace pipeline
