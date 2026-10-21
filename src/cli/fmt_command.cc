// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/fmt_command.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fmt/formatter.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "pipeline/pipeline_context.h"

namespace cli {

namespace {

// The name input arriving on a pipe is reported under. A pipe carries no
// file behind it, so this is the honest label rather than a stand-in for
// a path the user could open.
constexpr std::string_view STDIN_NAME = "<stdin>";

// Reads standard input to its end. A pipe hands over whatever is ready, so
// the read count is the loop's condition and only a negative one is an
// error.
base::Result<std::string, std::string_view> read_stdin() {
  std::string text;
  // Growing until the end: a source file is not known to be small, and
  // one read cannot say how much is left.
  constexpr usize CHUNK = static_cast<usize>(64) * 1024;
  std::vector<char> buffer(CHUNK);
  while (true) {
    const isize got = io::read(io::STDIN_FD, buffer.data(), buffer.size());
    if (got < 0) {
      return base::make_err(std::string_view("cannot read standard input"));
    }
    if (got == 0) {
      return base::make_ok(std::move(text));
    }
    text.append(buffer.data(), static_cast<usize>(got));
  }
}

}  // namespace

ResultCode run_fmt(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope) {
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  // The one configuration the formatter has. Handed to it here and read
  // from nowhere else, so the same input lays out the same way on every
  // machine.
  const alcy_fmt::FormatConfig layout = alcy_fmt::default_format_config();
  const bool check_only = config.fmt_check;

  if (config.fmt_files.empty()) {
    // No file named: standard input to standard output, the shape an
    // editor buffer or a pipe hands the tool.
    base::Result<std::string, std::string_view> text = read_stdin();
    if (text.is_err()) {
      ctx.bag.emit_message(diag::Severity::Error,
                           i18n::format<i18n::Key::CliFmtReadFailed>(
                               config.language, STDIN_NAME));
      return ResultCode::FmtFailed;
    }
    std::string source = std::move(text).unwrap();
    const std::string formatted = alcy_fmt::format(source, layout);
    envelope.file_count = 1;
    if (check_only) {
      if (formatted != source) {
        ctx.bag.emit_message(diag::Severity::Error,
                             i18n::format<i18n::Key::CliFmtNotFormatted>(
                                 config.language, STDIN_NAME));
        return ResultCode::FmtNotClean;
      }
      envelope.status = Status::Ok;
      envelope.outcome = Outcome::Checked;
      return ResultCode::Success;
    }
    io::write(io::STDOUT_FD, formatted.data(), formatted.size());
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Piped;
    return ResultCode::Success;
  }

  bool failed = false;
  usize unclean = 0;
  for (const std::string_view named : config.fmt_files) {
    const std::string path(named);
    if (!io::is_file(path)) {
      ctx.bag.emit_message(
          diag::Severity::Error,
          i18n::format<i18n::Key::CliFmtReadFailed>(config.language, path));
      failed = true;
      continue;
    }
    const std::string source = io::read_file(path);
    const std::string formatted = alcy_fmt::format(source, layout);
    if (formatted == source) {
      // Already laid out; writing it back would only move the timestamp.
      continue;
    }
    if (check_only) {
      ctx.bag.emit_message(
          diag::Severity::Error,
          i18n::format<i18n::Key::CliFmtNotFormatted>(config.language, path));
      ++unclean;
      continue;
    }
    const std::span<const u8> bytes{
        reinterpret_cast<const u8*>(formatted.data()), formatted.size()};
    if (!io::write_file(bytes, path)) {
      ctx.bag.emit_message(
          diag::Severity::Error,
          i18n::format<i18n::Key::CliFmtWriteFailed>(config.language, path));
      failed = true;
    }
  }
  envelope.file_count = config.fmt_files.size();
  if (failed) {
    return ResultCode::FmtFailed;
  }
  if (unclean > 0) {
    return ResultCode::FmtNotClean;
  }
  envelope.status = Status::Ok;
  envelope.outcome = check_only ? Outcome::Checked : Outcome::Formatted;
  return ResultCode::Success;
}

}  // namespace cli
