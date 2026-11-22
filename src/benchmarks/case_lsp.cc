// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/case_lsp.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"
#include "lsp/analysis.h"
#include "lsp/analysis_cache.h"
#include "lsp/code_actions.h"
#include "lsp/document.h"
#include "lsp/hover.h"
#include "lsp/semantic_tokens.h"

namespace bench {

namespace {

// One rebuild of a package reading is milliseconds, so no batch: a
// batch would share the reading one run invalidates. The request cases
// are microseconds on a reading the prepare keeps warm, so they take
// the runner's own batching floor.
constexpr MeasurementPolicy REANALYZE = {
    .warmup = 3,
    .samples = 0,
    .min_duration_ns = 1000ull * 1000 * 1000,
    .min_batch_ns = 0,
};
constexpr MeasurementPolicy REQUEST = {
    .warmup = 3,
    .samples = 0,
    .min_duration_ns = 1000ull * 1000 * 1000,
    .min_batch_ns = 50000,
};

// The edited text: the generated source with one more function at the
// end. The shape of the change matters less than that the two versions
// differ and both are valid, which is all an edit asks the cache to
// see.
std::string edited(std::string text) {
  text += "\nfn lsp_edit_probe() -> i32 {\n  ret 1\n}\n";
  return text;
}

}  // namespace

void run_lsp_cases(Runner<SteadyClock>& runner,
                   const SourceSpec& spec,
                   const CaseFilter& filter,
                   Emitter& emit) {
  const std::string source = generate_source(spec);
  const lsp::Document document("file:///lsp_case.al", "alcy", 1, source);
  const lsp::Document edited_document("file:///lsp_case.al", "alcy", 2,
                                      edited(source));
  // A position inside the first function's body: the request cases need
  // somewhere to sit, and the head of the file is the one place every
  // generated source has.
  constexpr lsp::Position kCursor{3, 8};
  // The state a case prepares and its run reads. The prepare rebuilds
  // them per sample - the reading's arena is not reused across samples
  // - and the request cases keep theirs warm through a batch.
  std::unique_ptr<lsp::AnalysisCache> cache;
  std::unique_ptr<lsp::DocumentStore> store;
  const lsp::DocumentAnalysis* analysis = nullptr;

  if (filter.wants(CaseId{"lsp", "reanalyze"})) {
    emit(CaseId{"lsp", "reanalyze"}, REANALYZE,
         runner.measure(
             REANALYZE,
             [&] {
               // A fresh cache is the edit's own premise: the reading
               // the edit invalidates is gone, and the arena the last
               // reading filled is not reused.
               cache = std::make_unique<lsp::AnalysisCache>();
               static_cast<void>(cache->get(document));
             },
             [&] { static_cast<void>(cache->get(edited_document)); }));
  }

  if (filter.wants(CaseId{"lsp", "did-change"})) {
    emit(
        CaseId{"lsp", "did-change"}, REQUEST,
        runner.measure(
            REQUEST,
            [&] {
              store = std::make_unique<lsp::DocumentStore>();
              static_cast<void>(
                  store->open("file:///lsp_case.al", "alcy", 1, source));
            },
            [&] {
              lsp::Document* document_ptr = store->find("file:///lsp_case.al");
              const std::array<lsp::ContentChange, 1> change{lsp::ContentChange{
                  .has_range = false, .range = {}, .text = edited(source)}};
              document_ptr->apply(change, 2);
            }));
  }

  if (filter.wants(CaseId{"lsp", "hover"})) {
    emit(CaseId{"lsp", "hover"}, REQUEST,
         runner.measure(
             REQUEST,
             [&] {
               cache = std::make_unique<lsp::AnalysisCache>();
               analysis = &cache->get(document);
             },
             [&] {
               static_cast<void>(
                   lsp::hover_markdown(document, *analysis, kCursor));
             }));
  }

  if (filter.wants(CaseId{"lsp", "code-actions"})) {
    emit(CaseId{"lsp", "code-actions"}, REQUEST,
         runner.measure(
             REQUEST,
             [&] {
               cache = std::make_unique<lsp::AnalysisCache>();
               static_cast<void>(cache->get(document));
             },
             [&] {
               static_cast<void>(
                   lsp::code_actions(document, *cache, kCursor, kCursor));
             }));
  }

  if (filter.wants(CaseId{"lsp", "semantic-tokens"})) {
    emit(CaseId{"lsp", "semantic-tokens"}, REQUEST,
         runner.measure(
             REQUEST,
             [&] {
               cache = std::make_unique<lsp::AnalysisCache>();
               analysis = &cache->get(document);
             },
             [&] {
               static_cast<void>(lsp::semantic_tokens(document, *analysis));
             }));
  }
}

}  // namespace bench
