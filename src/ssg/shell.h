// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ssg/catalog.h"

namespace ssg {

// The shell's settings menu: one markup string for every page, built in
// the page's language. The menu holds the theme toggle and a dropdown
// of this page's language trees; the toggle's icons switch on
// `data-theme-mode`, which the runtime's site module owns.

struct LanguageOption {
  std::string language;
  std::string name;
  // This page's address in that tree, relative to the current page.
  std::string href;
  bool current = false;
};

struct SettingsOptions {
  const Catalog* catalog = nullptr;
  std::string_view language;
  std::vector<LanguageOption> languages;
};

std::string render_settings(const SettingsOptions& options);

}  // namespace ssg
