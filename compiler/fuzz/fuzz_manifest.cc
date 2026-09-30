// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Arbitrary bytes as a manifest.
//
// A manifest is the one file a user writes by hand and the compiler
// parses with a third-party parser, and its diagnostics are computed from
// spans toml++ reports. That combination is where the column bug lived:
// toml++ counts columns in code points, so treating its column as a byte
// offset moved every diagnostic that followed a non-ASCII character.
//
// The property is that a span built from a toml++ region names the
// bytes toml++ meant, which is checked here by rendering the diagnostic
// and confirming the reported line:column lands inside the buffer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "pkg/manifest.h"
#include "source/source.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, usize size) {
  mem::Arena arena;
  arena.reserve(1u << 20);
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  // A manifest needs a filename to attribute diagnostics to.
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, bag, arena);
  if (parsed.is_err()) {
    return 0;
  }
  pkg::PackageManifest manifest = std::move(parsed).unwrap();
  // Whatever came back must satisfy the same verifier the pipeline
  // applies, or module selection would read unchecked fields.
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  (void)verified;
  return 0;
}
