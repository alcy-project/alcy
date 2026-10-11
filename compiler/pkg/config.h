// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

namespace pkg {

// Directory beside alcy.toml holding the goal package's own build
// configuration. A package built as someone else's dependency never
// has this directory read; see docs/adr/0019-goal-config-in-dot-alcy.md.
constexpr std::string_view CONFIG_DIR_NAME = ".alcy";

}  // namespace pkg
