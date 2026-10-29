// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/engine.h"

#include <span>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "linter/bindings.h"
#include "linter/config.h"
#include "linter/context.h"
#include "linter/level.h"
#include "linter/rule.h"
#include "linter/walk.h"
#include "pipeline/pipeline_context.h"

namespace linter {

namespace {

// Whether the module is the goal package's own code. A staged prelude
// is the toolchain's and a path dependency's modules belong to the
// package that declares them; lint.toml configures the goal package
// alone, so neither is checked.
bool lintable(const analyzer::ModuleTree& tree, u32 module) {
  const analyzer::ModuleNode& node = *tree.modules[module];
  if (node.is_staged) {
    return false;
  }
  return tree.module_root(module) == analyzer::NO_PACKAGE_ROOT;
}

// A rule's position in the registry, which is the index the settings
// are read by. The active lists hold rows from the same array, so the
// difference is the position.
u32 rule_position(const Rule* rule) {
  return static_cast<u32>(rule - all_rules().data());
}

// The one expression walk's dispatch: every enabled expression rule
// sees each node, in registry order, with the context naming it.
void dispatch_expr(LintContext& ctx, ast::ExprIdx expr) {
  for (const Rule* rule : ctx.active_expr) {
    ctx.rule = rule;
    ctx.rule_index_value = rule_position(rule);
    rule->check_expr(ctx, expr);
  }
}

void dispatch_item(LintContext& ctx, ast::ItemIdx item) {
  for (const Rule* rule : ctx.active_item) {
    ctx.rule = rule;
    ctx.rule_index_value = rule_position(rule);
    rule->check_item(ctx, item);
  }
}

}  // namespace

LintStats run_lints(pipeline::PipelineContext& ctx,
                    const analyzer::CheckedPackage& package,
                    const LintConfig& config) {
  const u32 warnings_before = ctx.bag.warning_count();
  const u32 errors_before = ctx.bag.error_count();

  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "lint", "frontend");
  LintContext lint(package, ctx.ast, ctx.sources, config, ctx.bag,
                   ctx.bag.language());
  const std::span<const Rule> all = all_rules();

  // The rules that read a node are dispatched from one walk per node
  // family; the rest run themselves. The split is built once, because
  // the configuration does not change between modules.
  std::vector<const Rule*> expr_rules;
  std::vector<const Rule*> item_rules;
  std::vector<const Rule*> run_rules;
  bool needs_bindings = false;
  for (u32 index = 0; index < static_cast<u32>(all.size()); ++index) {
    if (config.setting(index).level == LintLevel::Allow) {
      continue;
    }
    const Rule& rule = all[index];
    needs_bindings = needs_bindings || rule.reads_bindings;
    if (rule.check_expr != nullptr) {
      expr_rules.push_back(&rule);
    } else if (rule.check_item != nullptr) {
      item_rules.push_back(&rule);
    } else {
      run_rules.push_back(&rule);
    }
  }
  lint.active_expr = std::span<const Rule*>(expr_rules);
  lint.active_item = std::span<const Rule*>(item_rules);

  BindingIndex bindings;
  for (u32 module = 0; module < package.tree.modules.size(); ++module) {
    if (!lintable(package.tree, module)) {
      continue;
    }
    lint.module = module;
    lint.binding_index = nullptr;
    if (needs_bindings) {
      collect_bindings(lint, bindings);
      lint.binding_index = &bindings;
    }
    if (!expr_rules.empty()) {
      for_each_expr(lint, &dispatch_expr);
    }
    if (!item_rules.empty()) {
      for_each_item(lint, &dispatch_item);
    }
    for (const Rule* rule : run_rules) {
      lint.rule = rule;
      lint.rule_index_value = rule_position(rule);
      rule->run(lint);
    }
  }
  return LintStats{
      .warnings = ctx.bag.warning_count() - warnings_before,
      .errors = ctx.bag.error_count() - errors_before,
  };
}

}  // namespace linter
