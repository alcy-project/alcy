// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "ir/type.h"

namespace codegen_llvm {

// What the backend builds for: the triple that names the machine, and the
// pointer width the IR was lowered against. One value, because the emitter,
// the optimizer, the object writer, and the runtime all have to agree about
// the machine - and they used to be three answers to one question, two of
// them picked by the host the compiler was built on rather than by the
// target it is building for.
struct Target {
  // Never empty: a caller with no target named asks `host_target`, which
  // resolves the host's triple once.
  std::string triple;
  ir::PointerWidth width = ir::PointerWidth::W64;

  // Whether the target's OS is Windows, which is what the runtime's libc
  // bindings key on: `_write` and `_aligned_malloc` rather than `write`
  // and `posix_memalign`.
  [[nodiscard]] bool is_windows() const;
};

// The host's default triple, resolved once by whoever is deciding what to
// build for. The width stays the caller's: this module answers what the
// machine is called, not how wide a pointer is on it.
[[nodiscard]] std::string host_triple();

}  // namespace codegen_llvm
