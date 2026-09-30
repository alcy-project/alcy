// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/case_frontend.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/parser.h"
#include "source/source.h"

namespace bench {

namespace {

// The source under test, and the token stream derived from it. Parsing
// reads the stream, so tokenizing is setup for that case and the thing
// measured for the other.
struct FrontendFixture {
  explicit FrontendFixture(std::string source_bytes)
      : bytes(std::move(source_bytes)), tokens_view(tokens) {
    arena.reserve(ARENA_CAPACITY);
  }

  std::string bytes;
  std::vector<lexer::Token> tokens;
  std::span<const lexer::Token> tokens_view;
  // Reserved once and left alone. Handing the space back between samples
  // would unmap and map it again on every one, and that churn is larger
  // and far noisier than the parse it surrounds. Growing to a high-water
  // mark on the first parse and staying there is also what the pipeline
  // does across the files of one invocation.
  mem::Arena arena;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  void tokenize() {
    tokens.clear();
    lexer::Lexer lexer(bytes, source::UNKNOWN_FILE, bag);
    lexer.tokenize(tokens);
    tokens_view = std::span<const lexer::Token>(tokens.data(), tokens.size());
  }

  // A whole generated file fits; the number is a bound, not a cost, and
  // only the pages a parse actually touches are ever committed.
  static constexpr usize ARENA_CAPACITY = static_cast<usize>(1) << 24;
};

// One parse. The arena is reused at its high-water mark and the AST
// arena is built per parse, which is what the pipeline does per file, so
// what is timed is the descent and the storage it needs.
void parse_once(FrontendFixture& fixture) {
  ast::AstArena ast;
  diag::DiagBag bag(fixture.arena, i18n::Language::EnUs);
  parser::Parser parser(fixture.tokens_view, fixture.bytes,
                        source::UNKNOWN_FILE, ast, bag);
  static_cast<void>(parser.parse());
}

}  // namespace

void run_frontend_cases(Runner<SteadyClock>& runner,
                        const SourceSpec& spec,
                        const CaseFilter& filter,
                        Emitter& emit) {
  FrontendFixture fixture(generate_source(spec));

  // A whole file is never short enough to need a batch, so both cases
  // ask for none: a batch here would hide the per-file cost that a user
  // actually pays. The duration floor is a second rather than a moment
  // because a median over twenty samples of a millisecond-long operation
  // moves as the machine breathes, and a benchmark whose own figure
  // wanders is not one anything can be concluded from.
  const MeasurementPolicy policy{
      .warmup = 3,
      .samples = 0,
      .min_duration_ns = 1000ull * 1000 * 1000,
      .min_batch_ns = 0,
  };

  if (filter.wants(CaseId{"frontend", "tokenize"})) {
    emit(CaseId{"frontend", "tokenize"}, policy,
         runner.measure(policy, [] {}, [&] { fixture.tokenize(); }));
  }

  if (filter.wants(CaseId{"frontend", "parse"})) {
    emit(CaseId{"frontend", "parse"}, policy,
         runner.measure(policy, [] {}, [&] { parse_once(fixture); }));
  }
}

}  // namespace bench
