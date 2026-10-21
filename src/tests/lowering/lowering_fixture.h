// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "lowering/lowering.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace lowering {

// The pipeline context a case builds its package in. The context owns
// the arena, the bag, and the interner the cases inspect. `name_capacity`
// caps the shared name table; zero takes its default, and a case that
// has to spend it asks for less.
struct Fixture {
  explicit Fixture(usize name_capacity = 0)
      : ctx{i18n::Language::EnUs, ast::AstArena::DEFAULT_SPAN_CAPACITY,
            name_capacity} {}
  pipeline::PipelineContext ctx;
  ast::AstArena& ast = ctx.ast;
  diag::DiagBag& bag = ctx.bag;
  source::SourceManager& sources = ctx.sources;
  ir::SymbolTable& strings = ctx.strings;
};

// The sources one case declared, held in memory. It stands in for a
// scratch directory and keeps the `dir` name so the cases read the way
// they were written.
using VirtualDir = tests::DeclaredSources;

// Records the sources rather than writing them, so a case cannot fail for
// a reason other than what it asserts.
inline void write_all(
    VirtualDir& dir,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        files) {
  for (const auto& [name, bytes] : files) {
    dir.add(name, bytes);
  }
}

// A package that resolved and checked, before lowering consumes it: a
// case that also runs the AST engine reads the checked package first.
struct CheckCase {
  std::optional<analyzer::CheckedPackage> checked;
  bool ok = false;
};

inline CheckCase check_case(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    Fixture& f,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude = {}) {
  std::vector<analyzer::ModuleInput> inputs;
  source::FileId root = source::UNKNOWN_FILE;
  std::deque<std::string> name_storage;
  for (std::string_view rel : rels) {
    std::optional<analyzer::ModuleInput> input = tests::register_source(
        f.sources, dir, rel, rel == root_rel, name_storage);
    if (!input.has_value()) {
      continue;
    }
    if (rel == root_rel) {
      root = input->id;
    }
    inputs.push_back(*input);
  }
  std::deque<std::string> prelude_storage;
  std::vector<analyzer::ModuleInput> prelude_inputs;
  for (const auto& [name, rel] : prelude) {
    const tests::VirtualSource* const file = dir.find(rel);
    if (file == nullptr) {
      continue;
    }
    prelude_storage.emplace_back(name);
    // A staged prelude source is a package facade, so its public
    // surface is in scope without a `use`.
    prelude_inputs.push_back({prelude_storage.back(),
                              f.sources.add_virtual(file->name, file->bytes),
                              true});
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree_result =
      pipeline::resolve_inputs(f.ctx, root, inputs, "testpkg", prelude_inputs);
  if (tree_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  analyzer::ModuleTree tree = std::move(tree_result).unwrap();
  base::Result<analyzer::CheckedPackage, diag::Reported> checked_result =
      analyzer::check_package(tree, ir::PointerWidth::W64, f.ast, f.bag,
                              f.strings);
  if (checked_result.is_err() || f.bag.has_errors()) {
    return {std::nullopt, false};
  }
  return {std::move(checked_result).unwrap(), true};
}

struct LowerCase {
  std::optional<LoweredPackage> lowered;
  bool ok;
};

inline LowerCase lower_checked_case(analyzer::CheckedPackage checked,
                                    Fixture& f) {
  base::Result<LoweredPackage, diag::Reported> lowered_result =
      lower_package(std::move(checked), ir::PointerWidth::W64, f.ast, f.strings,
                    f.bag, nullptr, &f.sources);
  if (lowered_result.is_err()) {
    return {std::nullopt, false};
  }
  return {std::move(lowered_result).unwrap(), !f.bag.has_errors()};
}

inline LowerCase lower_case(
    VirtualDir& dir,
    std::string_view root_rel,
    std::initializer_list<std::string_view> rels,
    Fixture& f,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        prelude = {}) {
  CheckCase checked = check_case(dir, root_rel, rels, f, prelude);
  if (!checked.ok || !checked.checked.has_value()) {
    return {std::nullopt, false};
  }
  return lower_checked_case(std::move(*checked.checked), f);
}

}  // namespace lowering
