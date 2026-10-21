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

// Everything the lowered form is a function of. A field that steers
// lowering belongs here; a field that does not must stay out, because
// the key decides whether the frontend runs at all. The profile and the
// emit mode do not steer it: the cached form is unoptimized, and every
// mode answers from one entry.
struct CacheInputs {
  // The loaded sources, with their content digests.
  ir::FileTable files;
  ir::PointerWidth width = ir::PointerWidth::W64;
  std::string_view compiler_version;
  std::string_view manifest;
  // The machine the build targets.
  std::string_view triple;
  // Which target of the package is being lowered: two targets read the
  // same sources and lower different trees, down to the entry point
  // they ask for and the module they root at.
  std::string_view target_name;
  bool is_lib = false;
  bool emit_entry = false;
  // The dependency summaries the run resolved against, one digest per
  // sidecar read. A summary steers lowering the way a source does.
  std::span<const base::Digest128> summary_digests;
};

// One digest over the inputs, in a fixed order: the identity over the
// loaded sources, the manifest's content hash, the triple, the target,
// and the summaries, sorted so resolution order cannot change it.
base::Digest128 cache_key(const CacheInputs& inputs);

// `<dir>/<32 hex>.irb`.
std::string cache_entry_path(std::string_view dir, base::Digest128 key);

// Loads the entry into a lowered package and installs its string table
// in the context, which the backend reads. Empty when there is no entry,
// when the entry was written from other inputs, or when it fails to
// load: a miss, never an error.
std::optional<lowering::LoweredPackage> cache_load(
    PipelineContext& ctx,
    std::string_view path,
    const base::Digest128& identity);

// Stores bytes under a temporary name and renames it into place, so a
// hit never reads a half-written file. False on any I/O failure; the
// build continues either way.
bool cache_store(std::string_view dir,
                 std::string_view path,
                 std::span<const u8> bytes);

}  // namespace pipeline
