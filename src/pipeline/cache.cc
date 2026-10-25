// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/cache.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "fpag/base/numeric.h"
#include "fpag/io/io_util.h"
#include "ir/deserialize.h"
#include "ir/summary.h"
#include "ir/write_input.h"
#include "lowering/lowering.h"
#include "pipeline/pipeline_context.h"

namespace {

// Both spellings separate on Windows; a backslash is an ordinary
// character anywhere else, where splitting on it too only revisits a
// name create_directory refuses.
bool is_separator(char ch) {
  return ch == '/' || ch == '\\';
}

// Creates every level of `dir`, best-effort and silent: a cache the
// build cannot write is a build without a cache, not an error.
bool create_directories(std::string_view dir) {
  for (usize i = 1; i < dir.size(); ++i) {
    if (!is_separator(dir[i])) {
      continue;
    }
    const std::string_view prefix = dir.substr(0, i);
    // "C:" is a drive root, not a directory to create.
    if (!prefix.empty() && prefix.back() == ':') {
      continue;
    }
    if (!io::create_directory(std::string(prefix), true)) {
      return false;
    }
  }
  return io::create_directory(std::string(dir), true);
}

}  // namespace

namespace pipeline {

base::Digest128 cache_key(const CacheInputs& inputs) {
  const base::Digest128 identity =
      ir::identity_digest(inputs.files, inputs.width, inputs.compiler_version);
  const base::Digest128 manifest_hash = base::hash128(inputs.manifest);
  std::vector<std::array<u8, 16>> summaries;
  summaries.reserve(inputs.summary_digests.size());
  for (const base::Digest128 digest : inputs.summary_digests) {
    summaries.push_back(digest.bytes());
  }
  std::sort(summaries.begin(), summaries.end());
  std::vector<u8> canonical;
  canonical.reserve(16 + 16 + inputs.triple.size() + inputs.target_name.size() +
                    summaries.size() * 16 + 2);
  for (const std::array<u8, 16> part :
       {identity.bytes(), manifest_hash.bytes()}) {
    canonical.insert(canonical.end(), part.begin(), part.end());
  }
  for (const char ch : inputs.triple) {
    canonical.push_back(static_cast<u8>(ch));
  }
  canonical.push_back(0);
  // The name is length-delimited: a name that ran into the next field
  // would key two different targets the same.
  const std::string target_length = std::to_string(inputs.target_name.size());
  for (const char ch : target_length) {
    canonical.push_back(static_cast<u8>(ch));
  }
  canonical.push_back(0);
  for (const char ch : inputs.target_name) {
    canonical.push_back(static_cast<u8>(ch));
  }
  canonical.push_back(inputs.is_lib ? 1 : 0);
  canonical.push_back(inputs.emit_entry ? 1 : 0);
  for (const std::array<u8, 16>& digest : summaries) {
    canonical.insert(canonical.end(), digest.begin(), digest.end());
  }
  return base::hash128(std::span<const u8>(canonical.data(), canonical.size()));
}

std::string cache_entry_path(std::string_view dir, base::Digest128 key) {
  std::string path(dir);
  path.push_back('/');
  path += key.hex();
  path += ".irb";
  return path;
}

std::optional<lowering::LoweredPackage> cache_load(
    PipelineContext& ctx,
    std::string_view path,
    const base::Digest128& identity) {
  const std::string bytes = io::read_file(std::string(path));
  if (bytes.empty()) {
    return std::nullopt;
  }
  base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(std::span<const u8>(
          reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
  if (loaded.is_err()) {
    return std::nullopt;
  }
  ir::LoadedIr entry = std::move(loaded).unwrap();
  // The entry records the identity it was written from. One written from
  // other inputs is a miss: a key collision, or an entry copied to the
  // wrong name, must not answer for this build.
  if (entry.identity != identity) {
    return std::nullopt;
  }
  // The backend reads the context's table, so the entry's table becomes
  // the context's: storage and strings travel together, and an entry
  // always agrees with the table beside it.
  ctx.strings = std::move(*entry.strings);
  std::vector<lowering::LoweredPackage::AddrInfo> addr_names;
  addr_names.reserve(entry.addr_names.size());
  for (const ir::LoadedIr::AddrNameEntry& name : entry.addr_names) {
    const std::optional<ir::SymbolTable::Id> id =
        ctx.strings.intern_copied(name.name);
    if (!id.has_value()) {
      return std::nullopt;
    }
    addr_names.push_back(
        {name.reg, ctx.strings.get(*id), name.is_param, name.is_capture});
  }
  // One run per nominal row, exactly as the writer laid them out. The
  // run is moved in one at a time - braces would copy it through an
  // initializer list - so it keeps the buffer the rows point into.
  std::vector<std::vector<ir::SummaryField>> summary_fields;
  summary_fields.push_back(std::move(entry.summary_fields));
  return lowering::LoweredPackage{std::move(entry.storage),
                                  std::move(entry.instr_spans),
                                  std::move(addr_names),
                                  entry.prelude_functions,
                                  0,
                                  0,
                                  std::move(entry.summary),
                                  std::move(summary_fields),
                                  // A stored entry was checked before it
                                  // was stored, and the key names the
                                  // compiler that checked it, so the
                                  // borrow pass needs no tables from it.
                                  {},
                                  {}};
}

bool cache_store(std::string_view dir,
                 std::string_view path,
                 std::span<const u8> bytes) {
  if (!create_directories(dir)) {
    return false;
  }
  const std::string staging = std::string(path) + ".part";
  if (!io::write_file(bytes, staging)) {
    return false;
  }
  if (!io::rename_file(staging, std::string(path))) {
    io::remove_file(staging);
    return false;
  }
  return true;
}

}  // namespace pipeline
