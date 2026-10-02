// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/fixture.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "benchmarks/generator.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "ir/type.h"
#include "lowering/lowering.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace bench {

CompilerFixture::CompilerFixture(SourceSpec spec)
    : CompilerFixture(generate_source(spec)) {}

CompilerFixture::CompilerFixture(std::string source)
    : source_(std::move(source)) {
  root_ = ctx_.sources.add_virtual("bench.al", source_);
  inputs_.push_back(analyzer::ModuleInput{"", root_});
}

bool CompilerFixture::failed() {
  // A diagnostic anywhere means the stage that produced it did not
  // finish, so the stages after it have nothing to hand on. A bag that
  // only warns is still a success: the pipeline gates on errors too.
  if (ctx_.bag.has_errors()) {
    ok_ = false;
  }
  return !ok_;
}

void CompilerFixture::resolve() {
  base::Result<analyzer::ModuleTree, diag::Reported> resolved =
      pipeline::resolve_inputs(
          ctx_, root_, std::span<const analyzer::ModuleInput>(inputs_), "");
  if (resolved.is_err() || failed()) {
    ok_ = false;
    return;
  }
  tree_ = std::move(resolved).unwrap();
}

void CompilerFixture::analyze() {
  if (!tree_.has_value()) {
    ok_ = false;
    return;
  }
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyzer::check_package(*tree_, ir::PointerWidth::W64, ctx_.ast, ctx_.bag,
                              ctx_.strings);
  if (checked.is_err() || failed()) {
    ok_ = false;
    return;
  }
  checked_ = std::move(checked).unwrap();
}

void CompilerFixture::lower() {
  if (!checked_.has_value()) {
    ok_ = false;
    return;
  }
  base::Result<lowering::LoweredPackage, diag::Reported> lowered =
      lowering::lower_package(std::move(*checked_), ir::PointerWidth::W64,
                              ctx_.ast, ctx_.strings, ctx_.bag);
  checked_.reset();
  if (lowered.is_err() || failed()) {
    ok_ = false;
    return;
  }
  lowered_ = std::move(lowered).unwrap();
}

}  // namespace bench
