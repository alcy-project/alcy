// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/arg/converter.h"
#include "fpag/base/result.h"
#include "fpag/term/color_mode.h"
#include "i18n/language.h"
#include "pipeline/emit_mode.h"
#include "pipeline/vcs.h"

template <>
struct arg::Converter<i18n::Language> {
  static base::Result<i18n::Language, arg::GetError> from_string(
      std::string_view v) {
    using arg::GetError, base::make_err, base::make_ok, i18n::Language;
    if (const std::optional<Language> found = i18n::language_from_tag(v);
        found.has_value()) {
      return make_ok(*found);
    }
    return make_err(GetError::InvalidArgument);
  }
};

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
    } else if (v == "llvm-bc") {
      return make_ok(EmitMode::LlvmBitcode);
    } else if (v == "ir") {
      return make_ok(EmitMode::Ir);
    } else {
      return make_err(GetError::InvalidArgument);
    }
  }
};

template <>
struct arg::Converter<codegen::Target> {
  static base::Result<codegen::Target, arg::GetError> from_string(
      std::string_view v) {
    using arg::GetError, base::make_err, base::make_ok, codegen::Target;
    const std::optional<Target> found = codegen::target_from_name(v);
    if (!found.has_value()) {
      return make_err(GetError::InvalidArgument);
    }
    return make_ok(*found);
  }
};

template <>
struct arg::Converter<codegen::Backend> {
  static base::Result<codegen::Backend, arg::GetError> from_string(
      std::string_view v) {
    using arg::GetError, base::make_err, base::make_ok, codegen::Backend;
    const std::optional<Backend> found = codegen::backend_from_name(v);
    if (!found.has_value()) {
      return make_err(GetError::InvalidArgument);
    }
    // Availability is the pipeline's to report: a backend this build does
    // not carry has no code to run, and the message says which build
    // would.
    return make_ok(*found);
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

