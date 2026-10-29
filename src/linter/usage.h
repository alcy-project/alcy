// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

namespace linter {

// The head segment of every path a module names outside its own use
// items, in traversal order. A path is how an import is used: a module
// import is the head of a multi-segment path and an item import is a
// path of its own, so one table answers both.
struct NameUsage {
  std::vector<std::string_view> names;
};

}  // namespace linter
