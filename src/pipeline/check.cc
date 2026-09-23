// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/check.h"

#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "borrow/borrow.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

namespace {

CheckResult ok(usize file_count, usize module_count, usize function_count) {
  return CheckResult{
      .file_count = file_count,
      .module_count = module_count,
      .function_count = function_count,
      .success = true,
  };
}

CheckResult err(usize file_count, usize module_count, usize function_count) {
  return CheckResult{
      .file_count = file_count,
      .module_count = module_count,
      .function_count = function_count,
      .success = false,
  };
}

}  // namespace

CheckResult finish_check(PipelineContext& ctx,
                         analyzer::ModuleTree tree,
                         usize file_count) {
  diag::Fallible<analyzer::CheckedPackage> checked =
      analyzer::check_package(tree, kTargetWidth, ctx.ast, ctx.bag);
  usize module_count = 0;
  usize function_count = 0;
  if (checked.is_err() || ctx.bag.has_errors()) {
    return err(file_count, 0, 0);
  } else {
    analyzer::CheckedPackage package = std::move(checked).unwrap();
    module_count = package.modules.size();
    diag::Fallible<lower::LoweredPackage> lowered = lower::lower_package(
        std::move(package), kTargetWidth, ctx.ast, ctx.strings, ctx.bag);
    if (lowered.is_err()) {
      return err(file_count, module_count, 0);
    } else {
      lower::LoweredPackage package_ir = std::move(lowered).unwrap();
      function_count = package_ir.storage.functions().size();
      borrow::check_borrows(package_ir, ctx.bag);
    }
  }

  if (!ctx.bag.has_errors()) {
    return ok(file_count, module_count, function_count);
  } else {
    return err(file_count, module_count, function_count);
  }
}

CheckResult check_package(PipelineContext& ctx,
                          const path::Path& root,
                          source::FileId manifest_file,
                          std::string_view manifest_name) {
  diag::Fallible<pkg::PackageManifest> parsed =
      pkg::parse_manifest(ctx.sources.bytes(manifest_file), manifest_name,
                          manifest_file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return err(0, 0, 0);
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();

  diag::Fallible<BinTarget> target =
      resolve_bin_target(ctx, root, manifest, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    return err(0, 0, 0);
  }
  BinTarget resolved = std::move(target).unwrap();
  return finish_check(ctx, resolved.tree, resolved.file_count);
}

}  // namespace pipeline
