// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/cache.h"

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
#include "ir/type.h"
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

base::Digest128 cache_key(const ir::FileTable& files,
                          ir::PointerWidth width,
                          std::string_view compiler_version,
                          std::string_view manifest,
                          std::string_view triple) {
  const base::Digest128 identity =
      ir::identity_digest(files, width, compiler_version);
  const base::Digest128 manifest_hash = base::hash128(manifest);
  std::vector<u8> canonical;
  canonical.reserve(16 + 16 + triple.size());
  for (const std::array<u8, 16> part :
       {identity.bytes(), manifest_hash.bytes()}) {
    canonical.insert(canonical.end(), part.begin(), part.end());
  }
  for (const char ch : triple) {
    canonical.push_back(static_cast<u8>(ch));
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

std::optional<lowering::LoweredPackage> cache_load(PipelineContext& ctx,
                                                   std::string_view path) {
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
  return lowering::LoweredPackage{
      std::move(entry.storage), std::move(entry.instr_spans),
      std::move(addr_names), entry.prelude_functions};
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
