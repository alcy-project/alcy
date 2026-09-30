// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Arbitrary path strings through `path::Path`.
//
// A package layout decides what reaches this API, and every module name
// in a build is derived from a path, so a path is attacker-influenced
// input like any other. The operations exercised here are the ones that
// once disagreed with each other: `from_native` canonicalizes, `join`
// appends and re-canonicalizes, and `is_absolute` and `parent` read the
// result.
//
// The property asserted is agreement, not a fixed answer: two spellings
// of the same path must compare equal, and a child joined to its parent
// must name the same place as spelling it out in one go. That is what
// module selection depends on, and a bare-string prefix comparison
// silently violated it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "fpag/base/result.h"
#include "path/path.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, usize size) {
  // Split the input in two so both halves are attacker-chosen.
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const usize split = bytes.find('\n');
  if (split == std::string_view::npos) {
    return 0;
  }
  const std::string_view head = bytes.substr(0, split);
  const std::string_view tail = bytes.substr(split + 1);

  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(head);
  if (root.is_err()) {
    return 0;
  }
  path::Path parent = std::move(root).unwrap();

  // Agreement: `parent / tail` must equal spelling the whole thing out.
  base::Result<path::Path, path::PathError> whole = path::Path::from_native(
      std::string(parent.as_view()) + "/" + std::string(tail));
  if (whole.is_ok()) {
    const path::Path joined = parent.join(tail);
    if (joined != std::move(whole).unwrap()) {
      // A disagreement is a defect in canonicalization, not a crash, so
      // it is reported the only way a fuzzer can: by aborting, which
      // libFuzzer turns into a minimizing reproducer.
      __builtin_trap();
    }
  }

  // These must not crash on any input, including a root-only path or a
  // path made entirely of separators.
  (void)parent.is_absolute();
  (void)parent.parent().as_view();
  return 0;
}
