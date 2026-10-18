// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "base/hash.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/binary_format.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/summary.h"
#include "ir/symbol_table.h"

namespace ir {

// The names a span's FileId resolves to, by FileId order. `hashes` is
// optional and parallel: a zero means the caller did not compute one.
struct FileTable {
  std::span<const std::string_view> names;
  std::span<const base::Digest128> hashes;

  [[nodiscard]] std::string_view name_of(u32 file) const {
    return file < names.size() ? names[file] : std::string_view{};
  }

  [[nodiscard]] base::Digest128 hash_of(u32 file) const {
    return file < hashes.size() ? hashes[file] : base::Digest128{};
  }
};

// One allocated place's name, as the borrow pass records it. The writers
// print it beside the alloca that defines the register.
struct AddrName {
  RegisterIdx reg;
  std::string_view name;
  bool is_param = false;
  bool is_capture = false;
};

// What both writers take: the storage and the side tables the pipeline
// holds beside it. `storage` and `strings` are required; an empty span
// means the package carries none of that piece.
struct WriteInput {
  const Storage* storage = nullptr;
  const ir::SymbolTable* strings = nullptr;
  // Parallel to the instruction table; empty when the package has none.
  std::span<const diag::Span> instr_spans;
  FileTable files;
  std::span<const AddrName> addr_names;
  // The package's public surface, for the summary a library's artifact
  // carries (ADR-0078). Empty when the caller has none.
  std::span<const SummaryEntry> summary;
  usize prelude_functions = 0;
  // The pointer width lowering ran with; the text header and the binary
  // header both record it.
  PointerWidth width = PointerWidth::W64;
  // The compiler build's version string, recorded by the binary form so
  // a cache can reject a mismatch. Empty when the caller has none.
  std::string_view compiler_version;
};

// The build identity: one digest over what an artifact was lowered from -
// the file table's names and content hashes in order, the pointer width,
// and the toolchain's version string. The binary form records it so a
// cache can compare one digest instead of walking the tables.
//
// The canonical bytes are: for each file row, in order, the name's bytes,
// one 0x00, and the hash as sixteen little-endian bytes; then one byte for
// the pointer width (4 or 8); then the version string's bytes.
//
// The profile is not in it: the unoptimized forms answer the same
// artifact under every profile (ADR-0067), so a cache that keys a
// profile's output composes the profile above this digest.
inline base::Digest128 identity_digest(const FileTable& files,
                                       PointerWidth width,
                                       std::string_view version) {
  std::vector<u8> canonical;
  for (usize i = 0; i < files.names.size(); ++i) {
    for (const char ch : files.names[i]) {
      canonical.push_back(static_cast<u8>(ch));
    }
    canonical.push_back(0);
    const std::array<u8, 16> hash = files.hash_of(static_cast<u32>(i)).bytes();
    canonical.insert(canonical.end(), hash.begin(), hash.end());
  }
  canonical.push_back(width == PointerWidth::W32 ? 4 : 8);
  for (const char ch : version) {
    canonical.push_back(static_cast<u8>(ch));
  }
  return base::hash128(std::span<const u8>(canonical.data(), canonical.size()));
}

}  // namespace ir
