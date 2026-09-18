// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "pkg/resolve.h"

namespace pkg {

// A resolved package frozen for lockfile emission. Views borrow arena
// storage owned by the caller of lock_resolved().
struct LockedPackage {
  std::string_view name;
  Version version;
  // Canonical source URI, e.g. "path+file:///abs/dir".
  std::string_view source;
};

struct Lockfile {
  const LockedPackage* packages = nullptr;
  u32 package_count = 0;
};

// Builds a lockfile model from resolved packages, in resolution order.
Lockfile lock_resolved(std::span<const ResolvedPackage> resolved,
                       mem::Arena& arena);

// Serializes in alcy.lock TOML format. Only '"' and '\\' and control
// characters are escaped; full TOML string fidelity is future work.
void serialize_lockfile(const Lockfile& lock, fmt::memory_buffer& out);

}  // namespace pkg
