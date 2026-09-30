// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "benchmarks/generator.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "fpag/mem/arena.h"
#include "fpag/str/string_interner.h"
#include "ir/storage.h"
#include "lower/lower.h"
#include "source/source.h"

namespace bench {

// The compiler state a case measures against, built up in stages.
//
// Each stage appends to the ones before it, so a case prepares what its
// own phase needs and times only that phase. The stages are rebuilt per
// sample rather than reused, because the analyzer and the lowerer
// consume what they are handed and the arena they allocate from would
// otherwise grow for the length of the run.
//
// The standard library is not staged. Staging it writes and maps every
// embedded member, and this fixture exists to keep the filesystem out of
// the measured path; a phase here therefore costs what it costs on a
// program's own source. A case that needs the suite is a process
// benchmark.
class CompilerFixture {
 public:
  explicit CompilerFixture(SourceSpec spec);

  // Takes the source directly, so a caller that is not a generated case
  // - a test, and nothing else - can hand over something the compiler
  // will not accept and see the fixture say so.
  explicit CompilerFixture(std::string source);

  // Fails quietly: a stage that reports leaves `ok()` false rather than
  // throwing, so a case's arithmetic is not obscured by a diagnostic
  // path the harness has no way to render.
  void resolve();
  void analyze();
  void lower();

  // False once any stage reported. For a generated source that means the
  // generator and the language have parted ways, and every figure taken
  // from here is about the wrong work.
  bool ok() const { return ok_; }

  // Each is a precondition rather than a lookup: a case asks for a stage
  // only after preparing the ones before it, and the check is there so
  // that asking wrongly stops the run instead of reading nothing.
  const analyzer::ModuleTree& tree() const {
    DCHECK(tree_.has_value());
    return *tree_;
  }
  const analyzer::CheckedPackage& checked() const {
    DCHECK(checked_.has_value());
    return *checked_;
  }
  const lower::LoweredPackage& lowered() const {
    DCHECK(lowered_.has_value());
    return *lowered_;
  }

  // The bag the stages wrote to. Borrow checking takes one by reference
  // even though it only reads, so the accessor hands out a mutable
  // reference: a case that reuses a package across samples reuses the
  // bag with it.
  diag::DiagBag& bag() { return bag_; }

  // The interner lowering interned names into, which the emitter reads
  // while it builds a module.
  str::StringInterner& strings() { return strings_; }

  // Hands over the verified storage, consuming the proof: the emitter
  // takes it by value, so one emission consumes it and a second needs a
  // package built again.
  ir::VerifiedStorage take_storage() {
    DCHECK(lowered_.has_value());
    return std::move(*lowered_).storage;
  }

 private:
  // Records that a stage reported, and says so. Not const: it is the
  // one place a failure is latched, so a caller cannot forget to.
  bool failed();

  std::string source_;
  source::FileId root_ = source::UNKNOWN_FILE;
  std::optional<analyzer::ModuleTree> tree_;
  std::optional<analyzer::CheckedPackage> checked_;
  std::optional<lower::LoweredPackage> lowered_;
  bool ok_ = true;

  // Declared ahead of the bag, which borrows the arena.
  mem::Arena arena_;
  ast::AstArena ast_;
  source::SourceManager sources_;
  diag::DiagBag bag_;
  str::StringInterner strings_;
  // The root module carries the empty name, as a single-file program's
  // does: the name is what an `import` would have to say, and a file
  // that imports nothing has none.
  std::vector<analyzer::ModuleInput> inputs_;
};

}  // namespace bench
