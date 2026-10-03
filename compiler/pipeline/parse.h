// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

// One file after lexing, parsing, and desugaring: the items the parser
// produced, and the canonical path a diagnostic names it by. The items
// are the run arena's, which outlives this.
struct ParsedFile {
  source::FileId id = source::UNKNOWN_FILE;
  path::Path path;
  std::span<const ast::ItemIdx> items;
};

// The files one parse admitted, addressable by id. File ids are dense,
// so a lookup is an index rather than a search; a file that was never an
// input is absent.
struct ParsedFiles {
  static constexpr u32 NOT_PARSED = ~u32{0};

  std::vector<ParsedFile> files;
  // One entry per id the source manager has handed out: an index into
  // `files`, or NOT_PARSED.
  std::vector<u32> by_id;

  const ParsedFile* find(source::FileId id) const {
    if (id >= by_id.size() || by_id[id] == NOT_PARSED) {
      return nullptr;
    }
    return &files[by_id[id]];
  }
};

// Lexes, parses, desugars, and verifies `files`, in order.
//
// Reading a file depends on nothing in another, so up to
// `ctx.parse_jobs()` threads take a share of it. While several do, each
// file reports into a bag of its own and the bags merge in input order,
// so the diagnostics a run reports do not depend on how many threads
// read the input; one thread writes into the run's bag directly, in the
// same order, and reserves nothing.
//
// The lowest-numbered file that stops the parse ends the run there. The
// arena is shared, so a file that did not fit leaves it as it was and
// every file after that one would report the same thing - taking the
// lowest index rather than whichever thread noticed first is what keeps
// that a property of the input.
//
// A parse does not know module names, so one parse serves every target
// of a package. A caller that resolves more than once pairs the result
// back with names using `parsed_module`.
base::Result<ParsedFiles, diag::Reported> parse_files(
    PipelineContext& ctx,
    std::span<const source::FileId> files);

// Pairs a module input with the items parsing produced for its file.
// The file must be one of the parsed inputs: the caller builds the input
// from them, so an id that is absent is a bug rather than an answer.
analyzer::ParsedModule parsed_module(const ParsedFiles& parsed,
                                     const analyzer::ModuleInput& input);

// Parses the inputs and resolves them in one step, for a caller that
// resolves once. A caller that resolves the same files more than once -
// a package's every target does - parses with `parse_files` and calls
// `analyzer::resolve_modules` per target, so the files are read once.
// `dependencies` are the path dependencies the package loads from
// source, whose files parse alongside the inputs'.
base::Result<analyzer::ModuleTree, diag::Reported> resolve_inputs(
    PipelineContext& ctx,
    source::FileId root,
    std::span<const analyzer::ModuleInput> modules,
    std::string_view package_name,
    std::span<const analyzer::ModuleInput> prelude = {},
    std::span<const analyzer::StdHint> std_hints = {},
    std::span<const analyzer::DependencyPackage> dependencies = {});

}  // namespace pipeline
