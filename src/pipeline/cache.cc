// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/cache.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "config/build_config.h"
#include "fingerprint/fingerprint.h"
#include "fpag/base/numeric.h"
#include "fpag/io/io_util.h"
#include "ir/deserialize.h"
#include "ir/summary.h"
#include "ir/write_input.h"
#include "lowering/lowering.h"
#include "pipeline/pipeline_context.h"

namespace {

// A backslash separates only on Windows. Elsewhere it is an ordinary
// filename byte, and splitting on it would create directories the
// caller never named.
bool is_separator(char ch) {
  return ch == '/' || (BUILD_FLAG(IS_OS_WIN) && ch == '\\');
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
  fingerprint::Builder canonical;
  canonical.reserve(64 + inputs.triple.size() + inputs.target_name.size() +
                    summaries.size() * 16);
  canonical.digest(identity).digest(manifest_hash);
  canonical.bytes(inputs.triple).u8(0);
  // The name is length-delimited: a name that ran into the next field
  // would key two different targets the same.
  canonical.bytes(std::to_string(inputs.target_name.size())).u8(0);
  canonical.bytes(inputs.target_name);
  canonical.u8(inputs.is_lib ? 1 : 0)
      .u8(inputs.emit_entry ? 1 : 0)
      .u8(inputs.testing ? 1 : 0)
      .u8(inputs.freestanding ? 1 : 0);
  for (const std::array<u8, 16>& digest : summaries) {
    canonical.bytes(std::string_view(
        reinterpret_cast<const char*>(digest.data()), digest.size()));
  }
  return canonical.finish();
}

std::string cache_entry_path(std::string_view dir, base::Digest128 key) {
  std::string path(dir);
  path.push_back('/');
  path += key.hex();
  path += ".irb";
  return path;
}

base::Digest128 object_cache_key(const ObjectCacheInputs& inputs) {
  std::vector<u8> canonical;
  canonical.reserve(16 + 4 + inputs.backend.size());
  for (const std::array<u8, 16> part : {inputs.lowered.bytes()}) {
    canonical.insert(canonical.end(), part.begin(), part.end());
  }
  canonical.push_back(inputs.optimize ? 1 : 0);
  canonical.push_back(inputs.overflow_checks ? 1 : 0);
  canonical.push_back(inputs.backtrace ? 1 : 0);
  for (const char ch : inputs.backend) {
    canonical.push_back(static_cast<u8>(ch));
  }
  canonical.push_back(0);
  return base::hash128(std::span<const u8>(canonical.data(), canonical.size()));
}

std::string object_entry_path(std::string_view dir, base::Digest128 key) {
  std::string path(dir);
  path.push_back('/');
  path += key.hex();
  path += ".obj";
  return path;
}

std::vector<u8> object_cache_serialize(const base::Digest128& key,
                                       std::span<const std::vector<u8>> parts) {
  std::vector<u8> blob;
  constexpr char kMagic[8] = {'A', 'L', 'C', 'Y', 'O', 'B', 'J', '1'};
  blob.insert(blob.end(), kMagic, kMagic + 8);
  const auto put_u32 = [&blob](u32 value) {
    for (usize shift = 0; shift < 32; shift += 8) {
      blob.push_back(static_cast<u8>((value >> shift) & 0xFF));
    }
  };
  const auto put_u64 = [&blob](u64 value) {
    for (usize shift = 0; shift < 64; shift += 8) {
      blob.push_back(static_cast<u8>((value >> shift) & 0xFF));
    }
  };
  const std::array<u8, 16> key_bytes = key.bytes();
  blob.insert(blob.end(), key_bytes.begin(), key_bytes.end());
  put_u32(static_cast<u32>(parts.size()));
  for (const std::vector<u8>& part : parts) {
    put_u64(part.size());
  }
  for (const std::vector<u8>& part : parts) {
    blob.insert(blob.end(), part.begin(), part.end());
  }
  return blob;
}

std::optional<std::vector<std::vector<u8>>> object_cache_load(
    const ObjectCacheEntry& entry) {
  const std::string raw = io::read_file(entry.path);
  const base::Digest128& key = entry.key;
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(raw.data()),
                                  raw.size());
  constexpr usize kHeader = 8 + 16 + 4;
  if (bytes.size() < kHeader) {
    return std::nullopt;
  }
  constexpr char kMagic[8] = {'A', 'L', 'C', 'Y', 'O', 'B', 'J', '1'};
  if (std::equal(kMagic, kMagic + 8, bytes.begin())) {
  } else {
    return std::nullopt;
  }
  if (std::memcmp(bytes.data() + 8, key.bytes().data(), 16) != 0) {
    return std::nullopt;
  }
  const usize count = static_cast<usize>(bytes[24]) |
                      (static_cast<usize>(bytes[25]) << 8) |
                      (static_cast<usize>(bytes[26]) << 16) |
                      (static_cast<usize>(bytes[27]) << 24);
  if (count == 0 || count > 4096 || bytes.size() < kHeader + count * 8) {
    return std::nullopt;
  }
  std::vector<std::vector<u8>> parts;
  parts.reserve(count);
  usize at = kHeader + count * 8;
  for (usize part = 0; part < count; ++part) {
    const usize base = kHeader + part * 8;
    usize size = 0;
    for (usize shift = 0, i = 0; shift < 64; shift += 8, ++i) {
      size |= static_cast<u64>(bytes[base + i]) << shift;
    }
    // Subtract instead of adding, so a size near the end of the address
    // space cannot wrap the comparison and pass as in bounds.
    if (size == 0 || size > bytes.size() - at) {
      return std::nullopt;
    }
    parts.emplace_back(bytes.begin() + static_cast<isize>(at),
                       bytes.begin() + static_cast<isize>(at + size));
    at += size;
  }
  return parts;
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
                                  std::move(entry.summary),
                                  std::move(summary_fields),
                                  // A stored entry was checked before it
                                  // was stored, and the key names the
                                  // compiler that checked it, so the
                                  // lowering's origin table is not
                                  // rebuilt. The trait tables ride with
                                  // the entry: the borrow pass reads
                                  // them, and a re-serialization writes
                                  // the bytes the fresh build wrote
                                  {},
                                  std::move(entry.not_movable),
                                  std::move(entry.needs_drop)};
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
