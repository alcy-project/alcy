// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/lint_command.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "linter/config.h"
#include "linter/diag_code.h"
#include "linter/fix.h"
#include "linter/level.h"
#include "linter/lint.h"
#include "linter/rule.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"

namespace cli {

namespace {

// How many times a fix run may rewrite and lint again. A repair can
// reveal another one, and two rules that disagree could rewrite each
// other; the cap turns the second case into a report rather than a loop
// that never ends.
constexpr u32 MAX_FIX_PASSES = 4;

enum class TargetKind : u8 { File, Package, Suite };

// The invocation's target, held apart from the context that reads it:
// a fix pass builds a fresh context, so a file id cannot be carried
// between passes, but the root path and the manifest's own name can.
struct LintTarget {
  TargetKind kind = TargetKind::Package;
  std::string file;
  // The canonical root spelling for a package or suite. A path value is
  // rebuilt from it in each pass, so the target carries no path object
  // that a context would have to own.
  std::string root;
  std::string manifest_name;
};

// Resolves what the command was pointed at and loads the lint settings
// that belong to it: beside the working directory for a single file,
// beside the package or suite manifest otherwise.
base::Result<LintTarget, diag::Reported> resolve_target(
    pipeline::PipelineContext& ctx,
    const CliConfig& config,
    linter::LintConfig* lint_config) {
  if (!config.file.empty()) {
    base::Result<path::Path, path::PathError> cwd =
        path::Path::from_native(".");
    if (cwd.is_err()) {
      return base::make_err(diag::Reported{});
    }
    base::Result<linter::LintConfig, diag::Reported> loaded =
        linter::load_config(ctx, std::move(cwd).unwrap());
    if (loaded.is_err()) {
      return base::make_err(diag::Reported{});
    }
    *lint_config = std::move(loaded).unwrap();
    LintTarget target;
    target.kind = TargetKind::File;
    target.file = std::string(config.file);
    return base::make_ok(std::move(target));
  }

  const std::string_view raw_input = config.input.empty() ? "." : config.input;
  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, raw_input, true);
  if (probe.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(found.manifest);
  const bool suite =
      manifest_bytes.has_value() &&
      pkg::probe_manifest_kind(*manifest_bytes) == pkg::ManifestKind::Suite;
  base::Result<linter::LintConfig, diag::Reported> loaded =
      linter::load_config(ctx, found.root);
  if (loaded.is_err()) {
    return base::make_err(diag::Reported{});
  }
  *lint_config = std::move(loaded).unwrap();
  LintTarget target;
  target.kind = suite ? TargetKind::Suite : TargetKind::Package;
  target.root = std::string(found.root.as_view());
  target.manifest_name = std::move(found.manifest_name);
  return base::make_ok(std::move(target));
}

// Lints the resolved target in one context. The package and suite forms
// probe their manifest here rather than carrying the first pass's file
// id, which belongs to a context that has since been replaced.
base::Result<linter::LintOutcome, diag::Reported> lint_target(
    pipeline::PipelineContext& ctx,
    const LintTarget& target,
    const linter::LintConfig& config) {
  if (target.kind == TargetKind::File) {
    return linter::lint_single_file(ctx, target.file, config);
  }
  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, target.root, true);
  if (probe.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (target.kind == TargetKind::Suite) {
    return linter::lint_suite(ctx, found.root, found.manifest,
                              found.manifest_name, config);
  }
  return linter::lint_package(ctx, found.root, found.manifest,
                              found.manifest_name, config);
}

struct AppliedFixes {
  u32 edits = 0;
  // A file could not be written. The loop stops, because the next pass
  // would read the unchanged file and propose the same repair again.
  bool failed = false;
};

// Writes one pass's repairs. A file the source manager named but the
// file system does not hold - a virtual source in a case - is skipped
// rather than reported.
AppliedFixes apply_fixes(const std::vector<linter::FileEdits>& files,
                         diag::DiagBag& bag) {
  AppliedFixes applied;
  for (const linter::FileEdits& file : files) {
    if (!io::is_file(file.path)) {
      continue;
    }
    const std::string original = io::read_file(file.path);
    const linter::EditOutcome edit = linter::apply_edits(original, file.edits);
    if (edit.applied == 0) {
      continue;
    }
    const std::span<const u8> bytes(
        reinterpret_cast<const u8*>(edit.text.data()), edit.text.size());
    if (!io::write_file(bytes, file.path)) {
      const u32 index = bag.emit<i18n::Key::LintFixWriteFailed>(
          linter::LintCode::FixWriteFailed, file.path);
      (void)index;
      applied.failed = true;
      return applied;
    }
    applied.edits += static_cast<u32>(edit.applied);
  }
  return applied;
}

// Rewrites each pass's repairs and lints again, up to the cap. The
// passes use throwaway contexts: their diagnostics describe bytes that
// no longer exist once a repair lands, so only their repairs are read.
// The caller runs the reporting pass on the context it keeps.
u32 run_fix_passes(pipeline::PipelineContext& ctx,
                   const LintTarget& target,
                   const linter::LintConfig& config,
                   diag::DiagBag& bag) {
  u32 fixed = 0;
  for (u32 pass = 0; pass < MAX_FIX_PASSES; ++pass) {
    pipeline::PipelineContext pass_ctx(ctx.bag.language());
    pass_ctx.jobs = ctx.jobs;
    pass_ctx.target = ctx.target;
    pass_ctx.backend = ctx.backend;
    pass_ctx.version = ctx.version;
    if (lint_target(pass_ctx, target, config).is_err()) {
      break;
    }
    const std::vector<linter::FileEdits> files =
        linter::collect_fixes(pass_ctx);
    if (files.empty()) {
      break;
    }
    const AppliedFixes applied = apply_fixes(files, bag);
    fixed += applied.edits;
    if (applied.failed || applied.edits == 0) {
      break;
    }
  }
  return fixed;
}

void finish(const linter::LintOutcome& result, Envelope& envelope) {
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Linted;
  envelope.file_count = result.file_count;
  envelope.module_count = result.module_count;
  envelope.lint_count = result.warning_count + result.error_count;
}

// Applies the command-line level overrides to the resolved table. A
// name that matches no rule is the same typo lint.toml would report.
// Deny is applied last, so a rule that two flags name is the loudest.
bool apply_overrides(const CliConfig& config,
                     linter::LintConfig& lint_config,
                     diag::DiagBag& bag) {
  const auto apply = [&](const std::vector<std::string_view>& names,
                         linter::LintLevel level) {
    for (const std::string_view name : names) {
      if (!linter::apply_override(lint_config, name, level)) {
        const u32 index = bag.emit<i18n::Key::LintCliUnknownRule>(
            linter::LintCode::ConfigSemanticError, name);
        (void)index;
        return false;
      }
    }
    return true;
  };
  return apply(config.lint_allow, linter::LintLevel::Allow) &&
         apply(config.lint_warn, linter::LintLevel::Warn) &&
         apply(config.lint_deny, linter::LintLevel::Deny);
}

// The exit code a finished lint run takes: a denied finding is an error
// in the bag, and the verb fails when it has one. The command still
// filled the envelope, so the counts are reported either way.
ResultCode lint_result_code(const pipeline::PipelineContext& ctx) {
  if (ctx.bag.has_errors()) {
    return ResultCode::LintFailed;
  }
  return ResultCode::Success;
}

// The rule table as text, one rule per line: the id, its default level,
// and the summary. The id column is padded so a reader can scan the
// levels down a terminal.
std::string rules_text() {
  const std::span<const linter::Rule> rules = linter::all_rules();
  usize width = 0;
  for (const linter::Rule& rule : rules) {
    width = std::max(width, rule.id.size());
  }
  std::string out;
  for (const linter::Rule& rule : rules) {
    out += rule.id;
    out.append(width - rule.id.size(), ' ');
    out += "  ";
    out += linter::lint_level_name(rule.default_level);
    out += "  ";
    out += rule.summary;
    out += '\n';
  }
  return out;
}

// The same table as one JSON document. Rule ids, levels, and summaries
// are compile-time ASCII holding no character JSON escapes, so the
// fields are written directly.
std::string rules_json() {
  std::string out = R"({"version":1,"rules":[)";
  bool first = true;
  for (const linter::Rule& rule : linter::all_rules()) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += R"({"id":")";
    out += rule.id;
    out += R"(","default":")";
    out += linter::lint_level_name(rule.default_level);
    out += R"(","summary":")";
    out += rule.summary;
    out += R"("})";
  }
  out += "]}";
  return out;
}

}  // namespace

ResultCode run_lint(const CliConfig& config,
                    pipeline::PipelineContext& ctx,
                    Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;

  // The listing reads no package, so it answers before a target is
  // resolved. It writes its own result, so the reporter adds nothing
  // after it on standard output.
  if (config.lint_list_rules) {
    const std::string listing = config.json ? rules_json() : rules_text();
    io::write(io::STDOUT_FD, listing.data(), listing.size());
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Piped;
    return ResultCode::Success;
  }

  linter::LintConfig lint_config{};
  base::Result<LintTarget, diag::Reported> target =
      resolve_target(ctx, config, &lint_config);
  if (target.is_err()) {
    return ResultCode::LintFailed;
  }
  const LintTarget resolved = std::move(target).unwrap();
  if (!apply_overrides(config, lint_config, ctx.bag)) {
    return ResultCode::LintFailed;
  }

  u32 fixed = 0;
  if (config.lint_fix) {
    fixed = run_fix_passes(ctx, resolved, lint_config, ctx.bag);
  }

  base::Result<linter::LintOutcome, diag::Reported> result =
      lint_target(ctx, resolved, lint_config);
  envelope.trace = trace.take_events();
  // The repairs landed even if the final pass found a new error, so the
  // count is recorded before the failure is returned.
  envelope.fix_count = fixed;
  if (result.is_err()) {
    return ResultCode::LintFailed;
  }
  finish(std::move(result).unwrap(), envelope);
  return lint_result_code(ctx);
}

}  // namespace cli
