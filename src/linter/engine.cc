// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/engine.h"

#include <span>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "linter/config.h"
#include "linter/context.h"
#include "linter/rule.h"
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

}  // namespace

LintStats run_lints(pipeline::PipelineContext& ctx,
                    const analyzer::CheckedPackage& package,
                    const LintConfig& config) {
  const u32 warnings_before = ctx.bag.warning_count();
  const u32 errors_before = ctx.bag.error_count();

  LintContext lint(package, ctx.ast, ctx.sources, config, ctx.bag,
                   ctx.bag.language());
  const std::span<const Rule> all = all_rules();
  for (u32 module = 0; module < package.tree.modules.size(); ++module) {
    if (!lintable(package.tree, module)) {
      continue;
    }
    lint.module = module;
    for (u32 index = 0; index < static_cast<u32>(all.size()); ++index) {
      lint.rule = &all[index];
      lint.rule_index_value = index;
      if (!lint.enabled()) {
        continue;
      }
      all[index].run(lint);
    }
  }
  return LintStats{
      .warnings = ctx.bag.warning_count() - warnings_before,
      .errors = ctx.bag.error_count() - errors_before,
  };
}

}  // namespace linter
