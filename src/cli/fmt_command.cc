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

void print_diff(const std::string& path,
                const std::string& original,
                const std::string& formatted) {
  const std::vector<std::string> old_lines = split_lines(original);
  const std::vector<std::string> new_lines = split_lines(formatted);
  const usize n = old_lines.size();
  const usize m = new_lines.size();
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
  // Find first differing index.
  usize first = 0;
  while (first < n && first < m && old_lines[first] == new_lines[first]) {
    ++first;
  }
  usize last_old = n, last_new = m;
  while (last_old > first && last_new > first &&
         old_lines[last_old - 1] == new_lines[last_new - 1]) {
    --last_old;
    --last_new;
  }
  const usize ctx = 3;
  const usize begin_old = first > ctx ? first - ctx : 0;
  const usize begin_new = first > ctx ? first - ctx : 0;
  const usize end_old = std::min(n, last_old + ctx);
  const usize end_new = std::min(m, last_new + ctx);
  std::string out;
  out += "--- " + path + "\n";
  out += "+++ " + path + "\n";
  out += "@@ -" + std::to_string(begin_old + 1) + "," +
         std::to_string(end_old - begin_old) + " +" +
         std::to_string(begin_new + 1) + "," +
         std::to_string(end_new - begin_new) + " @@\n";
  for (usize i = begin_old; i < end_old; ++i) {
    if (i < first || i >= last_old) {
      if (i < n) {
        out += " " + old_lines[i] + "\n";
      }
    } else {
      out += "-" + old_lines[i] + "\n";
    }
  }
  for (usize i = begin_new; i < end_new; ++i) {
    if (i < first || i >= last_new) {
    } else {
      out += "+" + new_lines[i] + "\n";
    }
  }
  io::write(io::STDOUT_FD, out.data(), out.size());
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

ResultCode run_fmt(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope) {
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const alcy_fmt::FormatConfig layout = alcy_fmt::default_format_config();
  const bool check_only = config.fmt_check;

  // stdin mode
  if (config.fmt_stdin) {
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
    if (config.fmt_stdout) {
      io::write(io::STDOUT_FD, formatted.data(), formatted.size());
    } else {
      io::write(io::STDOUT_FD, formatted.data(), formatted.size());
    }
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

  bool failed = false;
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
    if (formatted == source) {
      continue;
    }
    if (check_only || config.fmt_diff) {
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
    if (config.fmt_stdout) {
      if (files.size() == 1) {
        io::write(io::STDOUT_FD, formatted.data(), formatted.size());
        continue;
      }
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
  envelope.outcome =
      check_only || config.fmt_diff ? Outcome::Checked : Outcome::Formatted;
  return ResultCode::Success;
}

}  // namespace cli
