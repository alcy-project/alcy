// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "fpag/base/result.h"
#include "ssg/catalog.h"

namespace ssg {

// The template stamper: the landing and the playground pages are
// templates stamped once per language at build time. `{{t:key}}` pulls
// a catalog string; `{{name}}` a value the caller computed. An
// unknown placeholder fails the build, so a page can never render with
// a hole in it.

base::Result<std::string, std::string> stamp_template(
    std::string_view source,
    const Catalog& catalog,
    std::string_view language,
    const std::unordered_map<std::string, std::string>& values);

}  // namespace ssg
