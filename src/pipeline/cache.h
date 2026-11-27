// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
  // Which artifact of the package is being lowered: two artifacts read
  // the same sources and lower different trees, down to the entry point
  // they ask for and the module they root at.
  std::string_view target_name;
  bool is_lib = false;
  bool emit_entry = false;
  // A test run parses a different tree from a build. The file table often
  // shows the difference through the run's driver, but the mode itself is
  // what steers lowering and belongs in the key on its own.
  bool testing = false;
  // A freestanding build parses and lowers a different tree: the entry is
  // _start and no C runtime is linked.
  bool freestanding = false;
  // The dependency summaries the run resolved against, one digest per
  // sidecar read. A summary steers lowering the way a source does.
  std::span<const base::Digest128> summary_digests;
};

// One digest over the inputs, in a fixed order: the identity over the
// loaded sources, the manifest's content hash, the triple, the target,
// and the summaries, sorted so resolution order cannot change it.
base::Digest128 cache_key(const CacheInputs& inputs);

// Where a target's cache entries live and what names them: the frontend
// entry's key and the identity its load checks, both over one read of
// the file table. Computed once per target and read by the lowering and
// by the emit stage, which salts the same key for its own entries.
struct FrontCache {
  std::string dir;
  base::Digest128 key;
  base::Digest128 identity;
};

// `<dir>/<32 hex>.irb`.
std::string cache_entry_path(std::string_view dir, base::Digest128 key);

// Everything the object bytes are a function of, past the lowered form
// the frontend entry holds: the profile bits and the backend that act
// between the cached IR and the machine code. Two builds that share one
// frontend entry keep separate object entries when these differ.
struct ObjectCacheInputs {
  // The frontend entry's key: the sources, the manifest, the target,
  // and the compiler version ride inside it.
  base::Digest128 lowered;
  bool optimize = false;
  bool overflow_checks = false;
  bool backtrace = false;
  std::string_view backend;
};

base::Digest128 object_cache_key(const ObjectCacheInputs& inputs);

// Where one target's object entry lives: the digest names it, the
// directory holds it, and the digest rides in the entry's header so a
// load can refuse a file that answers for other inputs.
struct ObjectCacheEntry {
  std::string dir;
  std::string path;
  base::Digest128 key;
};

// `<dir>/<32 hex>.obj`.
std::string object_entry_path(std::string_view dir, base::Digest128 key);

// The parts, back to back behind a header that names their sizes and
// the key they were written under.
std::vector<u8> object_cache_serialize(const base::Digest128& key,
                                       std::span<const std::vector<u8>> parts);

// The stored parts, or empty when there is no entry, the header does
// not parse, or the key inside is not the one asked for: a miss, never
// an error.
std::optional<std::vector<std::vector<u8>>> object_cache_load(
    const ObjectCacheEntry& entry);

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
