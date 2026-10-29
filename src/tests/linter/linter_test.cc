// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "linter/config.h"
#include "linter/fix.h"
#include "linter/lint.h"
#include "path/path.h"
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

// The repair a finding of one rule carries, or null when it offers
// none. A case asserts the bytes a client would write, not just that a
// repair exists.
const diag::Fix* fix_for(const diag::DiagBag& bag, std::string_view rule) {
  const diag::Fix* fix = nullptr;
  bag.for_each([&](const diag::Diagnostic& finding) {
    if (finding.check_name == rule) {
      fix = finding.fix;
    }
  });
  return fix;
}

// A two-module package under proj/: the manifest names main and util,
// and a case supplies both files. Returns the unused-import findings,
// or nullopt when setup or linting failed.
constexpr std::string_view IMPORT_MANIFEST =
    "[package]\nname = \"demo\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"main\", \"util\"]\n\n"
    "[[bin]]\nname = \"demo\"\npath = \"main.al\"\n";

std::optional<u32> unused_imports(io::TempDir& dir,
                                  std::string_view util,
                                  std::string_view main) {
  if (!dir.write_file("proj/alcy.toml", IMPORT_MANIFEST) ||
      !dir.write_file("proj/util.al", util) ||
      !dir.write_file("proj/main.al", main)) {
    return std::nullopt;
  }
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.join("proj"));
  if (root.is_err()) {
    return std::nullopt;
  }
  const path::Path root_path = std::move(root).unwrap();
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(root_path.join("alcy.toml").as_view());
  if (manifest.is_err()) {
    return std::nullopt;
  }
  base::Result<LintConfig, diag::Reported> config =
      parse_lint_config("", "<test>", source::UNKNOWN_FILE, ctx.bag, ctx.arena);
  if (config.is_err()) {
    return std::nullopt;
  }
  base::Result<LintOutcome, diag::Reported> result =
      lint_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                   std::move(config).unwrap());
  if (result.is_err()) {
    return std::nullopt;
  }
  return findings_for(ctx.bag, "unused-import");
}

struct Run {
  pipeline::PipelineContext ctx;
  LintOutcome outcome{};

  Run() : ctx(i18n::Language::EnUs) {}

  // Runs without asserting success, for a case that checks the failure
  // itself.
  bool attempt(std::string_view source, const LintConfig& config) {
    base::Result<LintOutcome, diag::Reported> result =
        lint_source(ctx, "test.al", source, config);
    if (result.is_err()) {
      return false;
    }
    outcome = std::move(result).unwrap();
    return true;
  }

  void operator()(std::string_view source, const LintConfig& config) {
    CHECK(attempt(source, config));
  }
};

}  // namespace

TEST_CASE("unused-import reports an import no path names") {
  io::TempDir dir = io::TempDir::create_unique("alcy_linter_import_");
  const std::optional<u32> unused = unused_imports(
      dir, "pub fn helper() -> i32 {\n  ret 1\n}\n",
      "use self::util::helper\n\nfn main() -> i32 {\n  ret 0\n}\n");
  CHECK(unused.has_value());
  if (unused.has_value()) {
    CHECK(*unused == 1);
  }
}

TEST_CASE("unused-import leaves an import a value path names") {
  io::TempDir dir = io::TempDir::create_unique("alcy_linter_import_used_");
  const std::optional<u32> unused = unused_imports(
      dir, "pub fn helper() -> i32 {\n  ret 1\n}\n",
      "use self::util::helper\n\nfn main() -> i32 {\n  ret helper()\n}\n");
  CHECK(unused.has_value());
  if (unused.has_value()) {
    CHECK(*unused == 0);
  }
}

TEST_CASE("unused-import leaves a type only a generic bound names") {
  io::TempDir dir = io::TempDir::create_unique("alcy_linter_import_bound_");
  const std::optional<u32> unused =
      unused_imports(dir, "pub spec Show {\n  fn show(self: Self) -> i32;\n}\n",
                     "use self::util::Show\n\n"
                     "fn f<T: Show>(x: T) -> i32 {\n  ret 0\n}\n\n"
                     "fn main() -> i32 {\n  ret 0\n}\n");
  CHECK(unused.has_value());
  if (unused.has_value()) {
    CHECK(*unused == 0);
  }
}

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

TEST_CASE("useless-cast reports a cast to the value's own type") {
  Run run;
  run("fn main() -> i32 {\n  n := 1\n  m := n as i32\n  ret m\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "useless-cast") == 1);
  const diag::Fix* fix = fix_for(run.ctx.bag, "useless-cast");
  CHECK(fix != nullptr);
  if (fix != nullptr) {
    CHECK(fix->replacement == "n");
  }
}

TEST_CASE("float-equality is off until it is asked for") {
  Run run;
  const std::string_view source =
      "fn main() -> i32 {\n  a := 1.0f64\n  b := 2.0f64\n"
      "  if a == b {\n    ret 1\n  }\n  ret 0\n}\n";
  run(source, default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "float-equality") == 0);
  Run asked;
  asked(source, config_from(asked.ctx, "[rules]\nfloat-equality = \"warn\"\n"));
  CHECK(findings_for(asked.ctx.bag, "float-equality") == 1);
}

TEST_CASE("needless-borrow reports a borrow of a shared reference") {
  Run run;
  run("fn main() -> i32 {\n  n := 7\n  r := &n\n  s := &*r\n  ret *s\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "needless-borrow") == 1);
  const diag::Fix* fix = fix_for(run.ctx.bag, "needless-borrow");
  CHECK(fix != nullptr);
  if (fix != nullptr) {
    CHECK(fix->replacement == "r");
  }
}

TEST_CASE("lossy-cast reports a narrowing cast when asked") {
  Run run;
  const std::string_view source =
      "fn main() -> i32 {\n  big := 300i64\n  small := big as i32\n  ret "
      "small\n}\n";
  run(source, default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "lossy-cast") == 0);
  Run asked;
  asked(source, config_from(asked.ctx, "[rules]\nlossy-cast = \"warn\"\n"));
  CHECK(findings_for(asked.ctx.bag, "lossy-cast") == 1);
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

TEST_CASE("borrow-checks = false lints a program the borrow pass refuses") {
  const std::string_view source =
      "fn main() -> i32 {\n"
      "  mut n := 7\n"
      "  unused := 1\n"
      "  r := &n\n"
      "  m := &mut n\n"
      "  *m = 1\n"
      "  ret *r\n"
      "}\n";
  Run strict;
  CHECK(!strict.attempt(source, default_config(strict.ctx)));
  Run fast;
  CHECK(fast.attempt(source, config_from(fast.ctx, "borrow-checks = false\n")));
  CHECK(findings_for(fast.ctx.bag, "unused-variable") == 1);
}

TEST_CASE("a non-boolean borrow-checks is a config error") {
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  base::Result<LintConfig, diag::Reported> parsed =
      parse_lint_config("borrow-checks = \"yes\"\n", "lint.toml",
                        source::UNKNOWN_FILE, ctx.bag, ctx.arena);
  CHECK(parsed.is_err());
  CHECK(ctx.bag.has_errors());
}

TEST_CASE("while-true offers a machine-applicable repair") {
  Run run;
  run("fn main() -> i32 {\n  mut n := 0\n  while true {\n"
      "    n = n + 1\n    if n > 3 {\n      break\n    }\n  }\n"
      "  ret n\n}\n",
      default_config(run.ctx));
  CHECK(findings_for(run.ctx.bag, "while-true") == 1);
  const diag::Fix* fix = nullptr;
  run.ctx.bag.for_each([&](const diag::Diagnostic& finding) {
    if (finding.check_name == "while-true") {
      fix = finding.fix;
    }
  });
  CHECK(fix != nullptr);
  if (fix != nullptr) {
    CHECK(fix->replacement == "loop");
    // The repair covers the keyword and the condition it replaces.
    CHECK(fix->span.length == 10);
  }
}

TEST_CASE("apply_edits rewrites non-overlapping spans") {
  const std::string_view original = "abcdef";
  const std::array<TextEdit, 2> edits = {
      TextEdit{.offset = 1, .length = 2, .replacement = "X"},
      TextEdit{.offset = 4, .length = 1, .replacement = ""},
  };
  const EditOutcome out =
      apply_edits(original, std::span<const TextEdit>(edits));
  CHECK(out.text == "aXdf");
  CHECK(out.applied == 2);
}

TEST_CASE("apply_edits drops an edit that overlaps an earlier one") {
  const std::string_view original = "abcdef";
  // The second edit starts inside the first; applying it would write
  // bytes neither repair chose.
  const std::array<TextEdit, 2> edits = {
      TextEdit{.offset = 1, .length = 3, .replacement = "X"},
      TextEdit{.offset = 2, .length = 2, .replacement = "Y"},
  };
  const EditOutcome out =
      apply_edits(original, std::span<const TextEdit>(edits));
  CHECK(out.text == "aXef");
  CHECK(out.applied == 1);
}

TEST_CASE("apply_edits drops an edit outside the file") {
  const std::string_view original = "abc";
  const std::array<TextEdit, 1> edits = {
      TextEdit{.offset = 4, .length = 1, .replacement = "X"},
  };
  const EditOutcome out =
      apply_edits(original, std::span<const TextEdit>(edits));
  CHECK(out.text == "abc");
  CHECK(out.applied == 0);
}

TEST_CASE("collect_fixes names the file and the repair") {
  Run run;
  run("fn main() -> i32 {\n  while true {\n    break\n  }\n  ret 0\n}\n",
      default_config(run.ctx));
  const std::vector<FileEdits> files = collect_fixes(run.ctx);
  CHECK(files.size() == 1);
  if (files.size() == 1) {
    CHECK(files[0].path == "test.al");
    CHECK(files[0].edits.size() == 1);
    if (!files[0].edits.empty()) {
      CHECK(files[0].edits[0].replacement == "loop");
    }
  }
}

}  // namespace linter
