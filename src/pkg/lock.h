// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
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

enum class LockError : u8 {
  // A resolved package fails manifest verification (lock_resolved), or a
  // package entry is unusable (serialize_lockfile).
  InvalidPackage,
  // The lockfile model itself is inconsistent: a package count with no
  // package array.
  InvalidLockfile,
};

// Builds a lockfile model from resolved packages, in resolution order.
// Each package must pass verify_manifest first; failures return the
// structured error without emitting anything.
base::Result<Lockfile, LockError> lock_resolved(
    std::span<const ResolvedPackage> resolved,
    mem::Arena& arena);

// Serializes in alcy.lock TOML format. Only '"' and '\\' and control
// characters are escaped; full TOML string fidelity is future work.
// Rejects a structurally invalid lock model instead of reading through
// it.
base::Result<void, LockError> serialize_lockfile(const Lockfile& lock,
                                                 fmt::memory_buffer& out);

}  // namespace pkg
