// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "linter/config.h"
#include "linter/level.h"
#include "linter/rule.h"
#include "source/source.h"

namespace linter {

// What one rule run reads. The context is rebuilt for each module, so a
// rule addresses the module it is checking through this rather than
// through a parameter of its own; the alternative is every rule
// carrying the same argument list.
//
// Nothing here is owned: the package, the syntax arena, the sources,
// and the bag all outlive the run. The context is a view assembled by
// the engine.
struct LintContext {
  const analyzer::CheckedPackage& package;
  ast::AstArena& ast;
  const source::SourceManager& sources;
  const LintConfig& config;
  diag::DiagBag& bag;
  i18n::Language language;

  LintContext(const analyzer::CheckedPackage& checked,
              ast::AstArena& arena,
              const source::SourceManager& manager,
              const LintConfig& settings,
              diag::DiagBag& diagnostics,
              i18n::Language lang)
      : package(checked),
        ast(arena),
        sources(manager),
        config(settings),
        bag(diagnostics),
        language(lang) {}

  // The rule being run, and its index in the registry. Set by the
  // engine before run() is called; a rule reads them rather than
  // naming itself at every report.
  const Rule* rule = nullptr;
  u32 rule_index_value = NO_RULE;
  // The module being checked, and its file. A rule that walks the
  // package loops the modules itself and does not read these.
  u32 module = 0;

  [[nodiscard]] const ast::ItemNode& item_node(ast::ItemIdx id) const {
    return ast.items[id];
  }
  [[nodiscard]] const ast::ExprNode& expr_node(ast::ExprIdx id) const {
    return ast.exprs[id];
  }
  [[nodiscard]] const ast::StmtNode& stmt_node(ast::StmtIdx id) const {
    return ast.stmts[id];
  }
  [[nodiscard]] const ast::PatternNode& pattern_node(ast::PatternIdx id) const {
    return ast.patterns[id];
  }
  [[nodiscard]] const ast::TypeNode& type_node(ast::TypeIdx id) const {
    return ast.types[id];
  }
  [[nodiscard]] const ast::Block& block(ast::BlockIdx id) const {
    return ast.blocks[id];
  }
  [[nodiscard]] const ast::Literal& literal(ast::LiteralIdx id) const {
    return ast.literals[id];
  }

  // The bytes of the file a span names, or empty for a span the source
  // manager cannot resolve.
  [[nodiscard]] std::string_view file_bytes(diag::Span span) const {
    const std::optional<std::string_view> bytes = sources.bytes(span.file);
    return bytes.has_value() ? *bytes : std::string_view{};
  }

  // The module currently checked.
  [[nodiscard]] const analyzer::ModuleNode& module_node() const {
    return *package.tree.modules[module];
  }

  [[nodiscard]] RuleSetting setting() const {
    return config.setting(rule_index_value);
  }

  [[nodiscard]] bool enabled() const {
    return setting().level != LintLevel::Allow;
  }

  // Reports one finding at a span, composing the message from the
  // catalog. A rule whose level is allow emits nothing: the check still
  // ran, and the config is what decided silence. The severity follows
  // the level, so a deny is an error the run fails on.
  template <i18n::Key K, typename... Args>
  u32 report(diag::Span span, Args&&... args) {
    const LintLevel level = setting().level;
    if (level == LintLevel::Allow) {
      return bag.size();
    }
    const diag::Severity severity = level == LintLevel::Deny
                                        ? diag::Severity::Error
                                        : diag::Severity::Warning;
    return bag.emit_named<K>(severity, rule->code, rule->id, span,
                             std::forward<Args>(args)...);
  }

  // Attaches a note to a finding already reported, pointing at a second
  // span with a catalog message. Returns the index report() answered so
  // a rule can label what it just emitted; an allow already answered
  // with a size that names nothing.
  template <i18n::Key K, typename... Args>
  void label(u32 finding, diag::Span span, Args&&... args) {
    if (finding >= bag.size()) {
      return;
    }
    fmt::memory_buffer out;
    i18n::format_to<K>(std::back_inserter(out), language,
                       std::forward<Args>(args)...);
    (void)bag.label_composed(finding, span, {out.data(), out.size()});
  }

  // Attaches a machine-applicable repair to a finding already reported:
  // the replacement is the bytes to put where the span is, empty to
  // delete them. A rule offers one only when it is sure of the repair.
  void fix(u32 finding, diag::Span span, std::string_view replacement) {
    if (finding >= bag.size()) {
      return;
    }
    (void)bag.attach_fix(finding, diag::Fix{span, replacement});
  }
};

}  // namespace linter
