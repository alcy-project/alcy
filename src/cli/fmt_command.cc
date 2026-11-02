// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/fmt_command.h"

#include <algorithm>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
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
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace cli {

namespace {

constexpr std::string_view STDIN_NAME = "<stdin>";

base::Result<std::string, std::string_view> read_stdin() {
  std::string text;
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

bool excluded(const std::string& path,
              const std::vector<std::string_view>& suffixes) {
  for (std::string_view suffix : suffixes) {
    if (!suffix.empty() && path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  usize start = 0;
  while (true) {
    const usize pos = text.find('\n', start);
    if (pos == std::string::npos) {
      out.push_back(text.substr(start));
      break;
    }
    out.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

// The lines a diff compares: a final newline ends the last line rather
// than starting an empty one, so the hunk a change at the end of the
// file falls in does not claim a line that is not there.
std::vector<std::string> diff_lines(const std::string& text) {
  std::vector<std::string> lines = split_lines(text);
  if (!text.empty() && text.back() == '\n') {
    lines.pop_back();
  }
  return lines;
}

// One step of the edit script: a line both layouts share, one only the
// original has, or one only the formatted layout has. line indexes the
// original for Keep and Delete and the formatted layout for Insert.
struct Edit {
  enum class Kind : u8 { Keep, Delete, Insert };
  Kind kind = Kind::Keep;
  usize line = 0;
};

// The lines of either side a hunk carries around its changes. Two
// changes closer than twice this share one hunk, or the hunks would
// print the same lines twice.
constexpr usize DIFF_CONTEXT = 3;

void print_diff(const std::string& path,
                const std::string& original,
                const std::string& formatted) {
  const std::string diff = render_diff(path, original, formatted);
  if (!diff.empty()) {
    io::write(io::STDOUT_FD, diff.data(), diff.size());
  }
}

base::Result<std::vector<std::string>, diag::Reported> collect_package_files(
    pipeline::PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    const std::vector<std::string_view>& excludes) {
  base::Result<std::vector<pipeline::Artifact>, diag::Reported> targets =
      pipeline::resolve_artifacts(ctx, root, manifest_file, manifest_name,
                                  pipeline::ArtifactScope::All);
  if (targets.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  std::vector<pipeline::Artifact> resolved = std::move(targets).unwrap();
  std::set<std::string> seen;
  std::vector<std::string> out;
  for (pipeline::Artifact& target : resolved) {
    for (analyzer::ModuleNode* module : target.tree.modules) {
      if (module->is_staged || module->is_prelude) {
        continue;
      }
      if (module->file == source::UNKNOWN_FILE) {
        continue;
      }
      std::optional<std::string_view> name = ctx.sources.name(module->file);
      if (!name.has_value()) {
        continue;
      }
      std::string path(*name);
      if (excluded(path, excludes)) {
        continue;
      }
      if (seen.insert(path).second) {
        out.push_back(std::move(path));
      }
    }
  }
  std::sort(out.begin(), out.end());
  return base::make_ok(std::move(out));
}

}  // namespace

// A unified diff of the two layouts, or nothing when they agree. Each
// hunk is a run of changes widened by the context it is read in, so a
// rewrite that touches two regions apart reports both instead of one
// block claiming every line between them changed.
std::string render_diff(std::string_view path,
                        std::string_view original,
                        std::string_view formatted) {
  const std::vector<std::string> old_lines = diff_lines(std::string(original));
  const std::vector<std::string> new_lines = diff_lines(std::string(formatted));
  const usize n = old_lines.size();
  const usize m = new_lines.size();
  // dp[i][j] is the length of the longest run of lines the two layouts
  // share from old_lines[i] and new_lines[j] on: the edit script is
  // whatever walks it downhill.
  std::vector<std::vector<usize>> dp(n + 1, std::vector<usize>(m + 1, 0));
  for (usize i = n; i-- > 0;) {
    for (usize j = m; j-- > 0;) {
      if (old_lines[i] == new_lines[j]) {
        dp[i][j] = dp[i + 1][j + 1] + 1;
      } else {
        dp[i][j] = std::max(dp[i + 1][j], dp[i][j + 1]);
      }
    }
  }
  std::vector<Edit> script;
  script.reserve(n + m);
  usize i = 0;
  usize j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && old_lines[i] == new_lines[j]) {
      script.push_back(Edit{Edit::Kind::Keep, i});
      ++i;
      ++j;
    } else if (j < m && (i == n || dp[i][j + 1] > dp[i + 1][j])) {
      script.push_back(Edit{Edit::Kind::Insert, j});
      ++j;
    } else {
      script.push_back(Edit{Edit::Kind::Delete, i});
      ++i;
    }
  }
  std::vector<usize> changes;
  for (usize k = 0; k < script.size(); ++k) {
    if (script[k].kind != Edit::Kind::Keep) {
      changes.push_back(k);
    }
  }
  std::string out;
  if (changes.empty()) {
    return out;
  }
  out += "--- " + std::string(path) + "\n";
  out += "+++ " + std::string(path) + "\n";
  usize read = 0;
  while (read < changes.size()) {
    usize last = read;
    while (last + 1 < changes.size() &&
           changes[last + 1] - changes[last] <= 2 * DIFF_CONTEXT) {
      ++last;
    }
    const usize begin =
        changes[read] > DIFF_CONTEXT ? changes[read] - DIFF_CONTEXT : 0;
    const usize end = std::min(script.size(), changes[last] + 1 + DIFF_CONTEXT);
    usize old_before = 0;
    usize new_before = 0;
    usize old_count = 0;
    usize new_count = 0;
    for (usize k = 0; k < end; ++k) {
      const Edit::Kind kind = script[k].kind;
      if (k < begin) {
        if (kind != Edit::Kind::Insert) {
          ++old_before;
        }
        if (kind != Edit::Kind::Delete) {
          ++new_before;
        }
        continue;
      }
      if (kind != Edit::Kind::Insert) {
        ++old_count;
      }
      if (kind != Edit::Kind::Delete) {
        ++new_count;
      }
    }
    out += "@@ -" + std::to_string(old_before + 1) + "," +
           std::to_string(old_count) + " +" + std::to_string(new_before + 1) +
           "," + std::to_string(new_count) + " @@\n";
    for (usize k = begin; k < end; ++k) {
      const Edit& edit = script[k];
      switch (edit.kind) {
        case Edit::Kind::Keep: out += " " + old_lines[edit.line] + "\n"; break;
        case Edit::Kind::Delete:
          out += "-" + old_lines[edit.line] + "\n";
          break;
        case Edit::Kind::Insert:
          out += "+" + new_lines[edit.line] + "\n";
          break;
      }
    }
    read = last + 1;
  }
  return out;
}

bool reads_stdin(const CliConfig& config) {
  // Standard input is a source only when `--stdin` names it;
  // naming no file means the package in the current directory.
  return config.fmt_stdin;
}

ResultCode run_fmt(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope) {
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const alcy_fmt::FormatConfig layout = alcy_fmt::default_format_config();
  const bool check_only = config.fmt_check;

  if (reads_stdin(config)) {
    base::Result<std::string, std::string_view> text = read_stdin();
    if (text.is_err()) {
      ctx.bag.emit_message(diag::Severity::Error,
                           i18n::format<i18n::Key::CliFmtReadFailed>(
                               config.language, STDIN_NAME));
      return ResultCode::FmtFailed;
    }
    const std::string source = std::move(text).unwrap();
    const std::string formatted = alcy_fmt::format(source, layout);
    envelope.file_count = 1;
    if (check_only || config.fmt_diff) {
      if (formatted != source) {
        if (config.fmt_diff) {
          print_diff(std::string(STDIN_NAME), source, formatted);
        } else {
          ctx.bag.emit_message(diag::Severity::Error,
                               i18n::format<i18n::Key::CliFmtNotFormatted>(
                                   config.language, STDIN_NAME));
        }
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

  // Build target list
  std::vector<std::string> files;
  if (!config.fmt_files.empty()) {
    for (std::string_view named : config.fmt_files) {
      std::string path(named);
      if (excluded(path, config.fmt_exclude)) {
        continue;
      }
      files.push_back(std::move(path));
    }
  } else {
    // package/suite discovery
    const std::string raw_input =
        config.input.empty() ? std::string(".") : std::string(config.input);
    base::Result<pipeline::ManifestProbe, diag::Reported> probe =
        pipeline::require_package_manifest(ctx, raw_input, true);
    if (probe.is_err()) {
      return ResultCode::FmtFailed;
    }
    pipeline::ManifestProbe found = std::move(probe).unwrap();
    const std::optional<std::string_view> manifest_bytes =
        ctx.sources.bytes(found.manifest);
    const bool suite =
        manifest_bytes.has_value() &&
        pkg::probe_manifest_kind(*manifest_bytes) == pkg::ManifestKind::Suite;
    if (suite) {
      base::Result<pkg::SuiteManifest, diag::Reported> parsed =
          pkg::parse_suite_manifest(*manifest_bytes, found.manifest_name,
                                    found.manifest, ctx.bag, ctx.arena);
      if (parsed.is_err()) {
        return ResultCode::FmtFailed;
      }
      pkg::SuiteManifest suite_manifest = std::move(parsed).unwrap();
      base::Result<void, pkg::SuiteError> verified =
          pkg::verify_suite_manifest(suite_manifest);
      if (verified.is_err()) {
        pkg::report_suite_error(std::move(verified).unwrap_err(),
                                found.manifest_name, ctx.bag);
        return ResultCode::FmtFailed;
      }
      if (suite_manifest.package_count == 0) {
        return ResultCode::Success;
      }
      for (u32 i = 0; i < suite_manifest.package_count; ++i) {
        base::Result<pipeline::ManifestProbe, diag::Reported> member_probe =
            pipeline::require_package_manifest(
                ctx, found.root.join(suite_manifest.packages[i]).as_view(),
                false);
        if (member_probe.is_err()) {
          return ResultCode::FmtFailed;
        }
        pipeline::ManifestProbe member = std::move(member_probe).unwrap();
        base::Result<std::vector<std::string>, diag::Reported> collected =
            collect_package_files(ctx, member.root, member.manifest,
                                  member.manifest_name, config.fmt_exclude);
        if (collected.is_err()) {
          return ResultCode::FmtFailed;
        }
        for (const std::string& f : std::move(collected).unwrap()) {
          files.push_back(f);
        }
      }
    } else {
      base::Result<std::vector<std::string>, diag::Reported> collected =
          collect_package_files(ctx, found.root, found.manifest,
                                found.manifest_name, config.fmt_exclude);
      if (collected.is_err()) {
        return ResultCode::FmtFailed;
      }
      files = std::move(collected).unwrap();
    }
  }

  // dedup
  {
    std::set<std::string> seen;
    std::vector<std::string> unique;
    for (const std::string& f : files) {
      if (seen.insert(f).second) {
        unique.push_back(f);
      }
    }
    files = std::move(unique);
  }

  if (files.empty()) {
    envelope.file_count = 0;
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Checked;
    return ResultCode::Success;
  }

  // A stream carries one file's layout, so a target that names or
  // discovers more than one is refused rather than written in place:
  // --stdout asked for the result on standard output.
  if (config.fmt_stdout && files.size() != 1) {
    ctx.bag.emit_message(
        diag::Severity::Error,
        i18n::format<i18n::Key::CliFmtStdoutTakesOneFile>(config.language));
    return ResultCode::ArgParseError;
  }

  bool failed = false;
  bool piped = false;
  usize unclean = 0;
  for (const std::string& named : files) {
    if (!io::is_file(named)) {
      ctx.bag.emit_message(
          diag::Severity::Error,
          i18n::format<i18n::Key::CliFmtReadFailed>(config.language, named));
      failed = true;
      continue;
    }
    const std::string source = io::read_file(named);
    const std::string formatted = alcy_fmt::format(source, layout);
    if (check_only || config.fmt_diff) {
      if (formatted == source) {
        continue;
      }
      if (config.fmt_diff) {
        print_diff(named, source, formatted);
      } else {
        ctx.bag.emit_message(diag::Severity::Error,
                             i18n::format<i18n::Key::CliFmtNotFormatted>(
                                 config.language, named));
      }
      ++unclean;
      continue;
    }
    // A layout that matches its source still leaves through the stream:
    // a reader of the pipe asked for the file's text, not for a change.
    if (config.fmt_stdout) {
      io::write(io::STDOUT_FD, formatted.data(), formatted.size());
      piped = true;
      continue;
    }
    if (formatted == source) {
      continue;
    }
    const std::span<const u8> bytes{
        reinterpret_cast<const u8*>(formatted.data()), formatted.size()};
    if (!io::write_file(bytes, named)) {
      ctx.bag.emit_message(
          diag::Severity::Error,
          i18n::format<i18n::Key::CliFmtWriteFailed>(config.language, named));
      failed = true;
    }
  }
  envelope.file_count = files.size();
  if (failed) {
    return ResultCode::FmtFailed;
  }
  if (unclean > 0) {
    return ResultCode::FmtNotClean;
  }
  envelope.status = Status::Ok;
  if (piped) {
    envelope.outcome = Outcome::Piped;
  } else {
    envelope.outcome =
        check_only || config.fmt_diff ? Outcome::Checked : Outcome::Formatted;
  }
  return ResultCode::Success;
}

}  // namespace cli
