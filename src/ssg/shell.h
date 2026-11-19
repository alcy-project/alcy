// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "ssg/catalog.h"

namespace ssg {

// The shell's settings menu: one markup string for every page, built in
// the page's language. The menu holds the theme toggle and the link to
// the same page in another language; the toggle's icons switch on
// `data-theme-mode`, which the runtime's site module owns.

struct SettingsOptions {
  const Catalog* catalog = nullptr;
  std::string_view language;
  // The URL of this page in the other language, as the page's reader
  // should see it (a relative href).
  std::string_view switch_href;
};

std::string render_settings(const SettingsOptions& options);

}  // namespace ssg
