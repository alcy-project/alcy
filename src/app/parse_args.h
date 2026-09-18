// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "app/driver_config.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/tagged_union.h"
#include "fpag/term/color_style.h"

namespace app {

enum class ParseInterruptedReason : u8 {
  ParseError,
  HelpRequested,
  VersionRequested,
  UnknownSubcommand,
};

using ParseArgsResult =
    base::AutoTaggedUnion<DriverConfig, ParseInterruptedReason>;

arg::Parser build_parser();

ParseArgsResult parse_args(arg::Parser&& parser,
                           i32 argc,
                           const char* const* argv,
                           term::ColorStyle style);

}  // namespace app
