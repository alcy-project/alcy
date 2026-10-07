// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace ir {

// Why a binary form could not be read. Anything finer would be a
// position in a file no one can open, so the kind is the whole message.
enum class IrLoadError : u8 {
  // The bytes end in the middle of a value.
  Truncated,
  BadMagic,
  // A major version this reader does not know. A newer minor is not an
  // error: unknown sections are skipped.
  UnsupportedVersion,
  // The flags do not describe a form this reader understands.
  BadFlags,
  // The section table names the same kind twice, overlaps, or leaves the
  // file.
  BadSection,
  // A count, index, or range that cannot address the table it names.
  OutOfRange,
  // A record whose payload does not fit its section.
  BadShape,
  // The rebuilt storage failed the verifier.
  Verification,
};

// One read-back package: verified storage, the strings its ids resolve
// in, and the side tables the writers carried. No view points into the
// input bytes, so a caller may drop them.
struct LoadedIr {
  struct AddrNameEntry {
    RegisterIdx reg;
    std::string name;
    bool is_param = false;
    bool is_capture = false;
  };

  // The verified storage has no default constructor, so the reader
  // gathers the side tables first and hands the storage in last.
  explicit LoadedIr(VerifiedStorage storage);
  LoadedIr(LoadedIr&&) = default;
  LoadedIr& operator=(LoadedIr&&) = default;
  LoadedIr(const LoadedIr&) = delete;
  LoadedIr& operator=(const LoadedIr&) = delete;

  VerifiedStorage storage;
  // The interner the storage's ids resolve in. Held by pointer because
  // the pool a StringInterner owns is not movable.
  std::unique_ptr<str::StringInterner> strings;
  std::vector<diag::Span> instr_spans;
  std::vector<std::string> file_names;
  std::vector<u64> file_hashes;
  std::vector<AddrNameEntry> addr_names;
  usize prelude_functions = 0;
  PointerWidth width = PointerWidth::W64;
  std::string compiler_version;
};

// Rebuilds verified IR from the bytes `serialize` wrote, or says why it
// could not. Only what the verifier accepts is handed out.
[[nodiscard]] base::Result<LoadedIr, IrLoadError> deserialize(
    std::span<const u8> bytes);

}  // namespace ir
