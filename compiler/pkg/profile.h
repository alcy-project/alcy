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
// docs/adr/0062-build-profiles-are-named-settings.md.
constexpr std::string_view PROFILE_FILE_NAME = "profile.toml";

// One profile table's keys. A key that is unset keeps whatever the
// profile it belongs to already had: the built-in value for `debug` and
// `release`, the baseline for a profile the file defines.
struct ProfileSettings {
  std::optional<bool> optimize;
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
