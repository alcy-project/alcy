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

// The field names the module reads, in traversal order: a `.name`
// projection on any expression, and a destructuring pattern's field.
// A constructor's initializer names are writes, not reads, so they are
// not recorded; that is the distinction unused-field reports on.
struct FieldUsage {
  std::vector<std::string_view> names;
};

// The variant names the package's value paths name, in traversal order:
// the last segment of every multi-segment path (`Color::Red` records
// `Red`). One table for the whole package, because a variant the
// declaring module never names may be another module's constructor;
// unused-variant reads it.
struct VariantUsage {
  std::vector<std::string_view> names;
};

}  // namespace linter
