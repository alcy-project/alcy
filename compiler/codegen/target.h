// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ir/type.h"

namespace codegen {

// What a backend builds for: the triple that names the machine, and the
// pointer width the IR was lowered against. One value, because the
// emitter, the object writer, and the runtime all have to agree about the
// machine - and they used to be three answers to one question, two of
// them picked by the host the compiler was built on rather than by the
// target it is building for.
//
// This lives here rather than in codegen_llvm because the pipeline needs
// the answer before a backend is chosen: a build with no LLVM still
// checks, and a check still knows what it is checking for.
struct Target {
  // Never empty: a caller with no target named asks `host_triple`, which
  // resolves the host's triple once.
  std::string triple;
  ir::PointerWidth width = ir::PointerWidth::W64;

  // Whether the target's OS is Windows, which is what the runtime's libc
  // bindings key on: `_write` and `_aligned_malloc` rather than `write`
  // and `posix_memalign`.
  [[nodiscard]] bool is_windows() const;

  // Whether the target is a wasm machine. A backend whose output is a
  // wasm module answers for these and not for the host.
  [[nodiscard]] bool is_wasm() const;

  // One value, so a caller that stores it can compare two of them.
  [[nodiscard]] bool operator==(const Target&) const = default;
};

// The host's default triple, resolved once by whoever is deciding what to
// build for. The width stays the caller's: this module answers what the
// machine is called, not how wide a pointer is on it.
[[nodiscard]] std::string host_triple();

// The host as a target: its triple and the pointer width its
// architecture gives pointers.
[[nodiscard]] Target host_target();

// The names a `--target` spelling may take, in the order help lists them.
// `host` is always one; the rest are machines a backend in this source
// tree can be asked for.
[[nodiscard]] std::vector<std::string> target_names();

// Resolves one of those names. Nothing for a spelling the compiler does
// not know; the caller says why it cannot build for it.
[[nodiscard]] std::optional<Target> target_from_name(std::string_view name);

}  // namespace codegen
