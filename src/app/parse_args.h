// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/driver_config.h"
#include "fpag/arg/parse_error.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/tagged_union.h"

namespace app {

// Pure command-line parsing: no I/O, no formatting. Rendering the outcome
// (help text, error messages) is the caller's job; see parse_output.h.
struct HelpRequested {};
struct VersionRequested {};
// Bare invocation with no subcommand. Render the root help for this.
struct NoSubcommand {};
struct UnknownSubcommand {
  std::string name;
};
struct ParseFailure {
  std::vector<arg::ParseError> errors;
};

using ParseOutcome = base::AutoTaggedUnion<DriverConfig,
                                           HelpRequested,
                                           VersionRequested,
                                           NoSubcommand,
                                           UnknownSubcommand,
                                           ParseFailure>;

arg::Parser build_parser();

// Parses args, where element 0 is the program name (same convention as
// argv). Views in the returned config borrow the input storage.
ParseOutcome parse_args(arg::Parser& parser,
                        std::span<const std::string_view> args);
ParseOutcome parse_args(arg::Parser& parser, i32 argc, const char* const* argv);

}  // namespace app
