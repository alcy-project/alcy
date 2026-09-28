// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/arg/converter.h"
#include "fpag/base/result.h"
#include "fpag/term/color_mode.h"
#include "pipeline/emit_mode.h"
#include "pipeline/vcs.h"

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

template <>
struct arg::Converter<pipeline::EmitMode> {
  static base::Result<pipeline::EmitMode, arg::GetError> from_string(
      std::string_view v) {
    using arg::GetError, base::make_err, base::make_ok, pipeline::EmitMode;
    if (v == "executable") {
      return make_ok(EmitMode::Executable);
    } else if (v == "object") {
      return make_ok(EmitMode::Object);
    } else if (v == "llvm-ir") {
      return make_ok(EmitMode::LlvmIr);
    } else {
      return make_err(GetError::InvalidArgument);
    }
  }
};

template <>
struct arg::Converter<pipeline::Vcs> {
  static base::Result<pipeline::Vcs, arg::GetError> from_string(
      std::string_view v) {
    using arg::GetError, base::make_err, base::make_ok, pipeline::Vcs;
    if (v == "git") {
      return make_ok(Vcs::Git);
    } else if (v == "none") {
      return make_ok(Vcs::None);
    } else {
      return make_err(GetError::InvalidArgument);
    }
  }
};

