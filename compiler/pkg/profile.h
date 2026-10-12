// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace pkg {

// Build profiles for the goal package. The file is optional; absent
// means the built-in profiles and nothing else. See
// docs/adr/0069-build-settings-live-in-named-profiles.md.
constexpr std::string_view PROFILE_FILE_NAME = "profile.toml";

// What a panic does before it ends the program: write its message, or
// also walk the frame chain and name the frames it finds. Both abort;
// the language has no unwinding either way.
enum class OnPanic : u8 { Abort, Backtrace };

// The two spellings the `on-panic` key accepts; nothing for another.
std::optional<OnPanic> on_panic_from_name(std::string_view name);

// One profile table's keys. A key that is unset keeps whatever the
// profile it belongs to already had: the built-in value for `debug` and
// `release`, the baseline for a profile the file defines.
struct ProfileSettings {
  std::optional<bool> optimize = std::nullopt;
  std::optional<OnPanic> on_panic = std::nullopt;
};

// One named table. The name is copied out of the parse's buffers, so it
// outlives the file bytes.
struct ProfileTable {
  std::string_view name;
  ProfileSettings settings;
};

// The tables a `.alcy/profile.toml` declared, in file order.
struct Profiles {
  std::span<const ProfileTable> tables;
};

// Parses `.alcy/profile.toml` bytes. Unknown keys are ignored, like
// alcy.toml; a mistyped known key is an error.
base::Result<Profiles, diag::Reported> parse_profile(std::string_view bytes,
                                                     std::string_view filename,
                                                     source::FileId /*file*/,
                                                     diag::DiagBag& bag,
                                                     mem::Arena& arena);

}  // namespace pkg
