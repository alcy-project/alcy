// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "analyzer/types.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "lsp/document.h"
#include "pipeline/pipeline_context.h"

namespace lsp {

// Runs the compiler frontend over one open buffer into context, and
// returns the checked package it borrows. The caller keeps context alive
// for as long as it reads the package.
//
// This is the one entry a semantic feature uses. It is the compiler's
// own path (front_end_root then analyze_tree), so an editor answer and a
// command-line answer cannot come from two readings of the source.
base::Result<analyzer::CheckedPackage, diag::Reported> analyze_document(
    pipeline::PipelineContext& context,
    const Document& document);

}  // namespace lsp
