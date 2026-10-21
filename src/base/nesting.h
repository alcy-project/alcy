// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "debug/dcheck.h"
#include "fpag/base/numeric.h"

namespace base {

// How deeply one pass may recurse over a single input. The grammar and
// the later tree walks are all recursive, so a pathologically nested
// file would otherwise exhaust the stack. The value is a property of the
// language, not of any one implementation, so every pass shares it: a
// program accepted at one limit is accepted at all of them.
//
// Sized well above anything hand-written (real code nests in single
// digits) and far enough below the smallest stack we build for that
// even the largest frame in the tree cannot exhaust it. Tests construct
// ASTs directly, so a pass may also meet a tree no file could spell.
inline constexpr u32 MAX_NESTING = 256;

// Tracks recursion depth against a budget. Spending the budget is a
// property of the input rather than an internal failure, so this only
// reports exhaustion; each consumer words the diagnostic in its own
// terms and picks its own code.
//
// Two counters, so `base` keeps its zero-allocation contract.
class NestingGuard {
 public:
  explicit NestingGuard(u32 limit) : limit_(limit) {}

  // True once the budget is spent. The caller must report and stop
  // recursing *before* entering, which keeps every deeper frame on the
  // same answer and the depth monotonic.
  bool exhausted() const { return depth_ >= limit_; }

  void enter() {
    DCHECK(!exhausted());
    ++depth_;
  }

  void leave() {
    DCHECK(depth_ != 0);
    --depth_;
  }

  u32 limit() const { return limit_; }

 private:
  u32 limit_;
  u32 depth_ = 0;
};

// Enters one level for the duration of a scope, so an early return
// cannot leak depth. Only construct after checking `exhausted()`.
class NestingScope {
 public:
  explicit NestingScope(NestingGuard& guard) : guard_(guard) { guard_.enter(); }
  ~NestingScope() { guard_.leave(); }

  NestingScope(const NestingScope&) = delete;
  NestingScope& operator=(const NestingScope&) = delete;

 private:
  NestingGuard& guard_;
};

}  // namespace base
