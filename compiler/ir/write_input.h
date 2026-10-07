// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/storage.h"

namespace ir {

// The names a span's FileId resolves to, by FileId order. `hashes` is
// optional and parallel: a zero means the caller did not compute one.
struct FileTable {
  std::span<const std::string_view> names;
  std::span<const u64> hashes;

  [[nodiscard]] std::string_view name_of(u32 file) const {
    return file < names.size() ? names[file] : std::string_view{};
  }

  [[nodiscard]] u64 hash_of(u32 file) const {
    return file < hashes.size() ? hashes[file] : 0;
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
  const str::StringInterner* strings = nullptr;
  // Parallel to the instruction table; empty when the package has none.
  std::span<const diag::Span> instr_spans;
  FileTable files;
  std::span<const AddrName> addr_names;
  usize prelude_functions = 0;
  // The pointer width lowering ran with; the text header and the binary
  // header both record it.
  PointerWidth width = PointerWidth::W64;
  // The compiler build's version string, recorded by the binary form so
  // a cache can reject a mismatch. Empty when the caller has none.
  std::string_view compiler_version;
};

}  // namespace ir
