// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/parse_args.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/cli_config.h"
#include "cli/converters.h"  // IWYU pragma: keep
#include "cli/suggest.h"
#include "cli/usage.h"
#include "cli/version.h"
#include "codegen/backend.h"
#include "codegen/target.h"
#include "debug/fatal.h"
#include "fpag/arg/arg.h"
#include "fpag/arg/command.h"
#include "fpag/arg/error_code.h"
#include "fpag/arg/matches.h"
#include "fpag/arg/parse_error.h"
#include "fpag/arg/parse_result.h"
#include "fpag/arg/parse_status.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/term/color_mode.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "pipeline/emit_mode.h"
#include "pipeline/vcs.h"

namespace cli {

namespace {

base::Result<CliConfig, ParseFailure> extract_from_matches(
    arg::Matches&& matches) {
  CliConfig c{};
  c.time_trace = matches.get<bool>("time-trace").unwrap_or(c.time_trace);
  c.json = matches.get<bool>("json").unwrap_or(c.json);
  // A count the parser cannot read is a rejected command line rather than a
  // default nobody asked for, so it is reported the way the parser reports
  // the rest. Zero is refused with it: no thread is not a count of one.
  if (matches.has("jobs")) {
    const std::string asked{
        matches.get<std::string_view>("jobs").unwrap_or("")};
    const u32 count = matches.get<u32>("jobs").unwrap_or(0);
    if (count == 0) {
      ParseFailure failure;
      failure.errors.emplace_back(arg::ErrorCode::InvalidValue, "--jobs",
                                  asked);
      return base::make_err(std::move(failure));
    }
    c.jobs = count;
  }
  c.color_mode = matches.get<term::ColorMode>("color").unwrap_or(c.color_mode);
  c.language = matches.get<i18n::Language>("lang").unwrap_or(c.language);

  const std::string_view selected = matches.selected_command();
  if (selected == "build") {
    c.subcommand = Subcommand::Build;
  } else if (selected == "compile") {
    c.subcommand = Subcommand::Compile;
  } else if (selected == "run") {
    c.subcommand = Subcommand::Run;
  } else if (selected == "new") {
    c.subcommand = Subcommand::New;
  } else if (selected == "init") {
    c.subcommand = Subcommand::Init;
  } else if (selected == "check") {
    c.subcommand = Subcommand::Check;
  }
  c.release = matches.get<bool>("release").unwrap_or(false);
  // The choices above already rejected anything else, so an unknown value
  // here means the default, not an error to report a second time.
  c.stdin_source = matches.get<bool>("stdin").unwrap_or(false);
  c.file = matches.get<std::string_view>("file").unwrap_or(c.file);
  c.emit = matches.get<pipeline::EmitMode>("emit").unwrap_or(
      pipeline::EmitMode::Executable);
  // A command that names no backend keeps None, and the pipeline resolves
  // it against the target; the converter already rejected a spelling this
  // build does not know.
  c.backend = matches.get<codegen::Backend>("backend").unwrap_or(c.backend);
  c.target = matches.get<codegen::Target>("target").unwrap_or(c.target);
  c.output = matches.get<std::string_view>("output").unwrap_or(c.output);
  c.linker = matches.get<std::string_view>("linker").unwrap_or(c.linker);
  if (auto link_args = matches.get_all<std::string_view>("link-args");
      link_args.is_ok()) {
    for (std::string_view argument : std::move(link_args).unwrap()) {
      if (!argument.empty()) {
        c.link_args.push_back(argument);
      }
    }
  }
  c.no_std = matches.get<bool>("no-std").unwrap_or(false);
  // `vcs` is declared on the two scaffolding verbs only, so the default
  // stands wherever it is absent.
  c.vcs = matches.get<pipeline::Vcs>("vcs").unwrap_or(c.vcs);
  if (auto deps = matches.get_all<std::string_view>("deps"); deps.is_ok()) {
    for (std::string_view fragment : std::move(deps).unwrap()) {
      if (!fragment.empty()) {
        c.deps.push_back(fragment);
      }
    }
  }

  const std::span<const std::string_view> positionals = matches.positionals();
  if (!positionals.empty()) {
    c.target_dir = positionals[0];
    c.program_args.assign(positionals.begin() + 1, positionals.end());
  }

  return base::make_ok(c);
}

ParseOutcome to_outcome(arg::Parser& parser,
                        std::span<const std::string_view> args,
                        arg::ParseResult<arg::Matches>&& result) {
  switch (result.status()) {
    case arg::ParseStatus::Success: {
      auto extracted = extract_from_matches(std::move(result).unwrap());
      if (extracted.is_err()) {
        return std::move(extracted).unwrap_err();
      }
      CliConfig config = std::move(extracted).unwrap();
      if (config.subcommand == Subcommand::None) {
        if (!config.target_dir.empty()) {
          UnknownSubcommand unknown{std::string(config.target_dir), ""};
          if (auto suggestion =
                  suggest_subcommand(parser.root_command(), unknown.name)) {
            unknown.suggestion = std::move(*suggestion);
          }
          return unknown;
        }
        return NoSubcommand{};
      }
      return config;
    }
    case arg::ParseStatus::Error: {
      ParseFailure failure{std::move(result).unwrap_err(), {}};
      for (usize i = 0; i < failure.errors.size(); ++i) {
        const arg::ParseError& error = failure.errors[i];
        if (error.code != arg::ErrorCode::UnknownLongOption) {
          continue;
        }
        if (auto suggestion =
                suggest_flag(parser.root_command(), args, error.context)) {
          failure.suggestions.push_back({i, std::move(*suggestion)});
        }
      }
      return failure;
    }
    case arg::ParseStatus::HelpRequested: return HelpRequested{};
    case arg::ParseStatus::VersionRequested: return VersionRequested{};
    default: UNREACHABLE();
  }
}

arg::CommandBuilder build_subcommand(std::string name, std::string about) {
  arg::CommandBuilder builder(std::move(name));
  builder.about(std::move(about));
  return builder;
}

arg::Arg vcs_arg(const UsageText& usage) {
  return arg::ArgBuilder("vcs")
      .help(usage.text(i18n::Key::CliVcsHelp))
      .choices({"git", "none"})
      .default_value("git")
      .build();
}

// The tags `--lang` accepts, taken from the catalog rather than written
// out here: a language the cli does not offer is a language the catalogs
// do not cover.
std::vector<std::string> language_choices() {
  std::vector<std::string> choices;
  choices.reserve(i18n::LANGUAGE_TAGS.size());
  for (const i18n::LanguageTag& entry : i18n::LANGUAGE_TAGS) {
    choices.emplace_back(entry.tag);
  }
  return choices;
}

}  // namespace

arg::Parser build_parser(i18n::Language language) {
  const UsageText& usage = usage_text(language);
  arg::CommandBuilder builder(ALCY_PROJECT_NAME, std::string(alcy_version()));
  builder.about(std::string(usage.text(i18n::Key::CliAbout)));
  builder.builtin_enabled(true);
  builder.add_arg(arg::ArgBuilder("color")
                      .help(usage.text(i18n::Key::CliColorHelp))
                      .default_value("auto")
                      .choices({"auto", "always", "never"})
                      .build());
  builder.add_arg(arg::ArgBuilder("lang")
                      .help(usage.text(i18n::Key::CliLangHelp))
                      .default_value(i18n::canonical_tag(i18n::Language::EnUs))
                      .choices(language_choices())
                      .build());
  builder.add_arg(arg::ArgBuilder("time-trace")
                      .short_name('t')
                      .help(usage.text(i18n::Key::CliTimeTraceHelp))
                      .is_flag(true)
                      .build());
  builder.add_arg(arg::ArgBuilder("json")
                      .help(usage.text(i18n::Key::CliJsonHelp))
                      .is_flag(true)
                      .build());
  builder.add_arg(arg::ArgBuilder("jobs")
                      .short_name('j')
                      .help(usage.text(i18n::Key::CliJobsHelp))
                      .value_name("N")
                      .build());
  builder.add_subcommand(
      build_subcommand("build",
                       std::string(usage.text(i18n::Key::CliBuildAbout)))
          .add_arg(arg::ArgBuilder("release")
                       .help(usage.text(i18n::Key::CliReleaseHelp))
                       .is_flag(true)
                       .build())
          .add_arg(arg::ArgBuilder("output")
                       .short_name('o')
                       .help(usage.text(i18n::Key::CliBuildOutputHelp))
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("emit")
                       .help(usage.text(i18n::Key::CliEmitHelp))
                       .choices({"executable", "object", "llvm-ir", "llvm-bc",
                                 "ir", "ir-bc"})
                       .default_value("executable")
                       .build())
          .add_arg(arg::ArgBuilder("backend")
                       .help(usage.text(i18n::Key::CliBackendHelp))
                       .choices({"llvm", "direct-wasm"})
                       .value_name("NAME")
                       .build())
          .add_arg(arg::ArgBuilder("target")
                       .help(usage.text(i18n::Key::CliTargetHelp))
                       .choices(codegen::target_names())
                       .value_name("TRIPLE")
                       .build())
          .add_arg(arg::ArgBuilder("linker")
                       .help(usage.text(i18n::Key::CliLinkerOverrideHelp))
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("link-args")
                       .help(usage.text(i18n::Key::CliLinkArgsOverrideHelp))
                       .default_value("")
                       .build())
          .build());
  builder.add_subcommand(
      build_subcommand("compile",
                       std::string(usage.text(i18n::Key::CliCompileAbout)))
          .add_arg(arg::ArgBuilder("release")
                       .help(usage.text(i18n::Key::CliReleaseHelp))
                       .is_flag(true)
                       .build())
          .add_arg(arg::ArgBuilder("output")
                       .short_name('o')
                       .help(usage.text(i18n::Key::CliCompileOutputHelp))
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("emit")
                       .help(usage.text(i18n::Key::CliEmitHelp))
                       .choices({"executable", "object", "llvm-ir", "llvm-bc",
                                 "ir", "ir-bc"})
                       .default_value("executable")
                       .build())
          .add_arg(arg::ArgBuilder("backend")
                       .help(usage.text(i18n::Key::CliBackendHelp))
                       .choices({"llvm", "direct-wasm"})
                       .value_name("NAME")
                       .build())
          .add_arg(arg::ArgBuilder("target")
                       .help(usage.text(i18n::Key::CliTargetHelp))
                       .choices(codegen::target_names())
                       .value_name("TRIPLE")
                       .build())
          .add_arg(arg::ArgBuilder("linker")
                       .help(usage.text(i18n::Key::CliCompileLinkerHelp))
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("link-args")
                       .help(usage.text(i18n::Key::CliCompileLinkArgsHelp))
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("stdin")
                       .help(usage.text(i18n::Key::CliStdinHelp))
                       .is_flag(true)
                       .build())
          .add_arg(arg::ArgBuilder("no-std")
                       .help(usage.text(i18n::Key::CliNoStdHelp))
                       .is_flag(true)
                       .build())
          .add_arg(arg::ArgBuilder("deps")
                       .help(usage.text(i18n::Key::CliDepsHelp))
                       .default_value("")
                       .build())
          .build());
  builder.add_subcommand(
      build_subcommand("run", std::string(usage.text(i18n::Key::CliRunAbout)))
          .add_arg(arg::ArgBuilder("release")
                       .help(usage.text(i18n::Key::CliReleaseHelp))
                       .is_flag(true)
                       .build())
          .add_arg(arg::ArgBuilder("linker")
                       .help(usage.text(i18n::Key::CliLinkerOverrideHelp))
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("link-args")
                       .help(usage.text(i18n::Key::CliLinkArgsOverrideHelp))
                       .default_value("")
                       .build())
          .build());
  builder.add_subcommand(
      build_subcommand("new", std::string(usage.text(i18n::Key::CliNewAbout)))
          .add_arg(vcs_arg(usage))
          .build());
  builder.add_subcommand(
      build_subcommand("init", std::string(usage.text(i18n::Key::CliInitAbout)))
          .add_arg(vcs_arg(usage))
          .build());
  builder.add_subcommand(
      build_subcommand("check",
                       std::string(usage.text(i18n::Key::CliCheckAbout)))
          .add_arg(arg::ArgBuilder("file")
                       .help(usage.text(i18n::Key::CliCheckFileHelp))
                       .default_value("")
                       .build())
          .build());
  return arg::Parser(std::move(builder).build());
}

ParseOutcome parse_args(arg::Parser& parser,
                        std::span<const std::string_view> args) {
  return to_outcome(parser, args, parser.try_parse(args));
}

ParseOutcome parse_args(arg::Parser& parser,
                        i32 argc,
                        const char* const* argv) {
  arg::ParseResult<arg::Matches> result = parser.try_parse(argc, argv);
  // Views over argv for flag suggestions, built only when an error may
  // carry one. The parser maps a null entry to an empty view, and so
  // does this.
  std::vector<std::string_view> args;
  if (result.status() == arg::ParseStatus::Error && argv != nullptr) {
    args.reserve(static_cast<usize>(argc > 0 ? argc : 0));
    for (i32 i = 0; i < argc; ++i) {
      args.emplace_back(argv[i] != nullptr ? std::string_view(argv[i])
                                           : std::string_view());
    }
  }
  return to_outcome(parser, args, std::move(result));
}

}  // namespace cli
