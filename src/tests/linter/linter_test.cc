// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "linter/config.h"
#include "linter/lint.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace linter {

namespace {

// The config an empty TOML document produces: every rule at its default
// level, which is what a case that does not care about configuration
// wants.
LintConfig default_config(pipeline::PipelineContext& ctx) {
  base::Result<LintConfig, diag::Reported> parsed =
      parse_lint_config("", "<test>", source::UNKNOWN_FILE, ctx.bag, ctx.arena);
  CHECK(parsed.is_ok());
  return std::move(parsed).unwrap();
}

LintConfig config_from(pipeline::PipelineContext& ctx, std::string_view toml) {
  base::Result<LintConfig, diag::Reported> parsed = parse_lint_config(
      toml, "<test>", source::UNKNOWN_FILE, ctx.bag, ctx.arena);
  CHECK(parsed.is_ok());
  return std::move(parsed).unwrap();
}

u32 findings_for(const diag::DiagBag& bag, std::string_view rule) {
  u32 count = 0;
  bag.for_each([&](const diag::Diagnostic& finding) {
    if (finding.check_name == rule) {
      ++count;
    }
  });
  return count;
}

// The source text a finding points at, which is what says the rule
// flagged the name it meant to.
std::string_view finding_text(const pipeline::PipelineContext& ctx,
                              std::string_view rule) {
  std::string_view text;
  ctx.bag.for_each([&](const diag::Diagnostic& finding) {
    if (text.empty() && finding.check_name == rule &&
        finding.has_primary_span) {
      const std::optional<std::string_view> bytes =
          ctx.sources.bytes(finding.primary_span.file);
      if (bytes.has_value()) {
        text = bytes->substr(finding.primary_span.offset,
                             finding.primary_span.length);
      }
    }
  });
  return text;
}

struct Run {
  pipeline::PipelineContext ctx;
  LintOutcome outcome{};

  Run() : ctx(i18n::Language::EnUs) {}

  void operator()(std::string_view source, const LintConfig& config) {
    base::Result<LintOutcome, diag::Reported> result =
        lint_source(ctx, "test.al", source, config);
    CHECK(result.is_ok());
    if (result.is_ok()) {
      outcome = std::move(result).unwrap();
    }
  }
};

}  // namespace

TEST_CASE("unused-variable names the binding") {
  Run run;
  run("fn main() -> i32 {\n  unused := 1\n  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "unused-variable") == 1);
  CHECK(finding_text(run.ctx, "unused-variable") == "unused");
}

TEST_CASE("a rule set to allow reports nothing") {
  Run run;
  run("fn main() -> i32 {\n  unused := 1\n  ret 0\n}\n",
      config_from(run.ctx, "[rules]\nunused-variable = \"allow\"\n"));
  CHECK(findings_for(run.ctx.bag, "unused-variable") == 0);
}

TEST_CASE("a rule set to deny fails the run") {
  Run run;
  run("fn main() -> i32 {\n  unused := 1\n  ret 0\n}\n",
      config_from(run.ctx, "[rules]\nunused-variable = \"deny\"\n"));
  CHECK(findings_for(run.ctx.bag, "unused-variable") == 1);
  CHECK(run.ctx.bag.has_errors());
  CHECK(run.outcome.error_count == 1);
}

TEST_CASE("unused-mut flags a mut that is only read") {
  Run run;
  run("fn main() -> i32 {\n  mut x := 1\n  println(\"{}\", x)\n  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "unused-mut") == 1);
  CHECK(finding_text(run.ctx, "unused-mut") == "x");
}

TEST_CASE("unused-mut accepts a reassigned binding") {
  Run run;
  run("fn main() -> i32 {\n  mut x := 1\n  x = 2\n  println(\"{}\", x)\n"
      "  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "unused-mut") == 0);
}

TEST_CASE("while-true reports a while over the literal true") {
  Run run;
  run("fn main() -> i32 {\n  while true {\n    break\n  }\n  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "while-true") == 1);
}

TEST_CASE("self-comparison reports identical operands") {
  Run run;
  run("fn main() -> i32 {\n  n := 1\n  if n == n {\n    println(\"x\")\n  }\n"
      "  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "self-comparison") == 1);
}

TEST_CASE("double-negation reports a repeated operator") {
  Run run;
  run("fn main() -> i32 {\n  b := true\n  if !!b {\n    println(\"x\")\n  }\n"
      "  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "double-negation") == 1);
}

TEST_CASE("nested-if reports an if whose only statement is an if") {
  Run run;
  run("fn main() -> i32 {\n  a := true\n  b := true\n  if a {\n    if b {\n"
      "      println(\"x\")\n    }\n  }\n  ret 0\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "nested-if") == 1);
}

TEST_CASE("unreachable-code reports the statement after a break") {
  Run run;
  // The rule is off by default because lowering warns about the same
  // expression, so the case turns it on.
  run("fn main() {\n  loop {\n    break\n    println(\"dead\")\n  }\n}\n",
      config_from(run.ctx, "[rules]\nunreachable-code = \"warn\"\n"));
  CHECK(findings_for(run.ctx.bag, "unreachable-code") == 1);
}

TEST_CASE("too-many-arguments reads its option") {
  Run run;
  const std::string_view source =
      "fn f(a: i32, b: i32) -> i32 {\n  ret a\n}\n"
      "fn main() -> i32 {\n  ret 0\n}\n";
  run(source, config_from(run.ctx,
                          "[rules]\ntoo-many-arguments = { level = \"warn\", "
                          "max-args = 1 }\n"));
  CHECK(findings_for(run.ctx.bag, "too-many-arguments") == 1);
}

TEST_CASE("missing-docs is off until it is asked for") {
  Run run;
  const std::string_view source =
      "pub fn documented_elsewhere() -> i32 {\n  ret 0\n}\n"
      "fn main() -> i32 {\n  ret 0\n}\n";
  run(source, default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "missing-docs") == 0);
  Run asked;
  asked(source, config_from(asked.ctx, "[rules]\nmissing-docs = \"warn\"\n"));
  CHECK(findings_for(asked.ctx.bag, "missing-docs") == 1);
}

TEST_CASE("an unknown rule is a config error") {
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  base::Result<LintConfig, diag::Reported> parsed =
      parse_lint_config("[rules]\nnot-a-rule = \"warn\"\n", "lint.toml",
                        source::UNKNOWN_FILE, ctx.bag, ctx.arena);
  CHECK(parsed.is_err());
  CHECK(ctx.bag.has_errors());
}

TEST_CASE("an unknown level is a config error") {
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  base::Result<LintConfig, diag::Reported> parsed =
      parse_lint_config("[rules]\nunused-variable = \"loud\"\n", "lint.toml",
                        source::UNKNOWN_FILE, ctx.bag, ctx.arena);
  CHECK(parsed.is_err());
  CHECK(ctx.bag.has_errors());
}

TEST_CASE("needless-return offers a machine-applicable repair") {
  Run run;
  run("fn main() -> i32 {\n  ret 1\n}\n", default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "needless-return") == 1);
  const diag::Fix* fix = nullptr;
  run.ctx.bag.for_each([&](const diag::Diagnostic& finding) {
    if (finding.check_name == "needless-return") {
      fix = finding.fix;
    }
  });
  CHECK(fix != nullptr);
  if (fix != nullptr) {
    CHECK(fix->replacement.empty());
    // The repair covers the keyword and the space before the value.
    CHECK(fix->span.length == 4);
  }
}

}  // namespace linter
