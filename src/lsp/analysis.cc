// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis.h"

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "lsp/document.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace lsp {

base::Result<analyzer::CheckedPackage, diag::Reported> analyze_document(
    pipeline::PipelineContext& context,
    const Document& document) {
  const source::FileId root =
      context.sources.add_virtual(document.uri(), document.text());
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      pipeline::front_end_root(context, root, pipeline::full_std_selection());
  if (tree.is_err()) {
    return base::make_err(diag::Reported{});
  }
  return pipeline::analyze_tree(context, tree.value());
}

}  // namespace lsp
