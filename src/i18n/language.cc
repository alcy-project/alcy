// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "i18n/language.h"

#include <string>

namespace i18n {

std::string tag_list() {
  std::string tags;
  for (const LanguageTag& entry : kLanguageTags) {
    if (!tags.empty()) {
      tags.push_back(' ');
    }
    tags.append(entry.tag);
  }
  return tags;
}

}  // namespace i18n
