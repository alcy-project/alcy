// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

#include "fpag/arg/converter.h"
#include "fpag/term/color_mode.h"

template <>
struct arg::Converter<term::ColorMode> {
  static base::Result<term::ColorMode, arg::GetError> from_string(
      std::string_view v) {
    using arg::GetError, term::ColorMode, base::make_err, base::make_ok;
    if (v == "auto") {
      return make_ok(ColorMode::Auto);
    } else if (v == "always") {
      return make_ok(ColorMode::Always);
    } else if (v == "never") {
      return make_ok(ColorMode::Never);
    } else {
      return make_err(GetError::InvalidArgument);
    }
  }
};

