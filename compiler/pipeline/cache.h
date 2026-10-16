// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/hash.h"
#include "fpag/base/numeric.h"
#include "ir/type.h"
#include "ir/write_input.h"

namespace lowering {
struct LoweredPackage;
}  // namespace lowering

namespace pipeline {

struct PipelineContext;

// What the cache did for a build: off when it was not consulted (a
// single file, or `--no-cache`), miss when it was consulted and the
// frontend ran, hit when the frontend was skipped.
enum class CacheOutcome : u8 { Off, Miss, Hit };

// A miss dominates: a build whose targets did not all hit reports the
// miss, so no invocation reads as cached when part of it was rebuilt.
constexpr CacheOutcome combine_cache_outcome(CacheOutcome first,
                                             CacheOutcome second) {
  if (first == CacheOutcome::Miss || second == CacheOutcome::Miss) {
    return CacheOutcome::Miss;
  }
  if (first == CacheOutcome::Hit || second == CacheOutcome::Hit) {
    return CacheOutcome::Hit;
  }
  return CacheOutcome::Off;
}

// One digest over what steers the lowered artifact, in a fixed order:
// the identity over the loaded sources, the pointer width, and the
// toolchain; the manifest's content hash; and the named target. The
// profile and the emit mode do not steer it: the cached form is
// unoptimized, and every mode answers from one entry.
base::Digest128 cache_key(const ir::FileTable& files,
                          ir::PointerWidth width,
                          std::string_view compiler_version,
                          std::string_view manifest,
                          std::string_view triple);

// `<dir>/<32 hex>.irb`.
std::string cache_entry_path(std::string_view dir, base::Digest128 key);

// Loads the entry into a lowered package and installs its string table
// in the context, which the backend reads. Empty when there is no entry
// or it fails to load: a miss, never an error.
std::optional<lowering::LoweredPackage> cache_load(PipelineContext& ctx,
                                                   std::string_view path);

// Stores bytes under a temporary name and renames it into place, so a
// hit never reads a half-written file. False on any I/O failure; the
// build continues either way.
bool cache_store(std::string_view dir,
                 std::string_view path,
                 std::span<const u8> bytes);

}  // namespace pipeline
