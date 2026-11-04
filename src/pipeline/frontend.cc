// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/frontend.h"

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "borrow/borrow.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "lowering/lowering.h"
#include "pipeline/embedded_std.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "source/source.h"

namespace pipeline {

base::Result<analyzer::CheckedPackage, diag::Reported> analyze_tree(
    PipelineContext& ctx,
    const analyzer::ModuleTree& tree) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "analyze", "frontend");
  const std::span<const analyzer::StdHint> hints(STD_HINTS, STD_HINT_COUNT);
  return analyzer::check_package(tree, ctx.target.width, ctx.ast, ctx.bag,
                                 ctx.strings, hints, ctx.profiler);
}

base::Result<FrontendOutput, diag::Reported> finish_frontend(
    PipelineContext& ctx,
    analyzer::CheckedPackage package) {
  // Staged prelude modules check with the package but read as
  // toolchain sources, so reported counts exclude them.
  const usize staged = package.tree.staged_modules;
  const usize module_count =
      package.modules.size() > staged ? package.modules.size() - staged : 0;
  // This half judges itself against the bag as it found it rather than
  // against emptiness. A caller that linted first has findings in the
  // bag already - a denied finding is an error - and those say nothing
  // about whether lowering worked.
  const u32 errors_before = ctx.bag.error_count();
  base::Result<lowering::LoweredPackage, diag::Reported> lowered = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "lower", "frontend");
    return lowering::lower_package(std::move(package), ctx.target.width,
                                   ctx.ast, ctx.strings, ctx.bag, ctx.profiler,
                                   &ctx.sources);
  }();
  if (lowered.is_err() || ctx.bag.error_count() > errors_before) {
    return base::make_err(diag::Reported{});
  }
  lowering::LoweredPackage package_ir = std::move(lowered).unwrap();
  // Reported counts exclude the parametric bodies, the specializations
  // of staged items, and the bodies the pass synthesized: no source
  // function stands for one (ADR-0079). The storage's lists are the
  // authority, so a loaded package counts what a freshly lowered one
  // does.
  const usize hidden = package_ir.prelude_functions +
                       package_ir.storage->parametric_functions().size() +
                       package_ir.storage->prelude_specializations().size() +
                       package_ir.storage->synthetic_functions().size();
  const usize function_count =
      package_ir.storage->functions().size() > hidden
          ? package_ir.storage->functions().size() - hidden
          : 0;
  const bool borrows_ok = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "borrow",
                                             "frontend");
    return borrow::check_borrows(package_ir, ctx.bag, ctx.strings, ctx.profiler)
        .is_ok();
  }();
  if (!borrows_ok || ctx.bag.error_count() > errors_before) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(FrontendOutput{
      .package = std::move(package_ir),
      .module_count = module_count,
      .function_count = function_count,
  });
}

base::Result<FrontendOutput, diag::Reported> run_frontend(
    PipelineContext& ctx,
    analyzer::ModuleTree tree) {
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_tree(ctx, tree);
  if (checked.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return finish_frontend(ctx, std::move(checked).unwrap());
}

base::Result<analyzer::ModuleTree, diag::Reported> front_end_root(
    PipelineContext& ctx,
    source::FileId root,
    const StdSelection& selection) {
  // A file that is one of the suite's own sources is checked as the member
  // it is: its path within the package names the modules above it, so the
  // tree hangs off the package the source belongs to instead of holding a
  // second copy of it beside a fileless root. Every other file names
  // nothing and is a root of its own.
  const std::optional<StagedMember> member = [&] {
    const std::optional<std::string_view> name = ctx.sources.name(root);
    if (!name.has_value()) {
      return std::optional<StagedMember>{};
    }
    return staged_member(*name, selection);
  }();
  const std::string_view member_path =
      member.has_value() ? member->path : std::string_view{};
  const analyzer::ModuleInput single_input{
      member_path, root, member.has_value() && member->is_facade, {}};
  std::vector<analyzer::ModuleInput> prelude = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "std-stage",
                                             "frontend");
    const std::span<const analyzer::ModuleInput> staged =
        std_prelude(ctx, selection);
    std::vector<analyzer::ModuleInput> excluding;
    excluding.reserve(staged.size());
    for (const analyzer::ModuleInput& input : staged) {
      // The member is the root of this tree; staging its copy as well
      // would put one source in two modules.
      if (input.name != member_path) {
        excluding.push_back(input);
      }
    }
    return excluding;
  }();
  // The staged members' seals are policy rows the same way a package
  // build's are; a single file has no manifest, so they come from the
  // embedded selection instead.
  const std::vector<analyzer::PackagePolicy> policies =
      std_package_policies(selection);
  // The root and every staged prelude source parse in one pass, so the
  // phase covers everything resolution then reads.
  // The resolver names nothing here: a single file has one module, so
  // the whole thing is one module-tree build and the scope covers that.
  return [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve-tree",
                                             "frontend");
    return resolve_inputs(ctx, root, {&single_input, 1}, "", prelude,
                          std_hints(), {}, policies);
  }();
}

bool has_free_main(const PipelineContext& ctx,
                   const analyzer::ModuleTree& tree) {
  for (ast::ItemIdx item : tree.modules[tree.root]->items) {
    const ast::ItemNode& node = ctx.ast.items[item];
    if (node.kind == ast::ItemKind::Fn &&
        node.payload.get<ast::ItemFn>().name.name == "main") {
      return true;
    }
  }
  return false;
}

}  // namespace pipeline
