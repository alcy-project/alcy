// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/parse.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "ast/lane.h"
#include "base/for_each.h"
#include "config/build_config.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/mem/arena.h"
#include "fpag/mem/page_allocator.h"
#include "i18n/messages.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/desugar.h"
#include "parser/parser.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

namespace {

// One file's diagnostics live in the bag its own thread built, over an
// arena the thread shares with the files it reads before and after. A bag
// only ever appends, and truncate only moves the bag's own count, so the
// bytes of a file already reported stay valid while the next file's are
// appended after them on the same arena.
struct PerFileDiagnostics {
  // Built by the thread that reads the file, because which worker's arena it
  // reports into is not known until then, and kept until the merge, because
  // what a file said is read in file order and not in the order it was said.
  std::unique_ptr<diag::DiagBag> bag;
  std::atomic<bool> ok{true};
};

// One worker's share of the reporting: the arena every file it reads
// reports into. This is per worker rather than per file because a file
// that reports nothing still reserved a whole arena of its own, and a
// package of a thousand files each doing that is what spreading the read
// costs. The arena is written by one thread only -- the worker that owns
// it -- so sharing it needs no synchronization; what shares it is ordered
// by the worker reading one file at a time.
//
// The reservation is one file's worth of messages times a few, not times
// the files: a clean file reports nothing, and a file that reports past
// what is left has its excess dropped and counted, which the run reports,
// the same answer the per-file arena gave.
#if BUILD_FLAG(IS_ARCH_64_BITS)
constexpr usize WORKER_DIAGNOSTIC_CAPACITY = 256ull << 10;
#else
constexpr usize WORKER_DIAGNOSTIC_CAPACITY = 128ull << 10;
#endif

struct PerWorkerDiagnostics {
  explicit PerWorkerDiagnostics() {
    // The arena takes whole pages, and a page is not always the 4 KiB the
    // constant assumes: a wasm page is 64 KiB, so the reservation rounds
    // up to the host's.
    const usize page = mem::page_size();
    arena.reserve((WORKER_DIAGNOSTIC_CAPACITY + page - 1) & ~(page - 1));
  }

  PerWorkerDiagnostics(const PerWorkerDiagnostics&) = delete;
  PerWorkerDiagnostics& operator=(const PerWorkerDiagnostics&) = delete;
  PerWorkerDiagnostics(PerWorkerDiagnostics&&) = delete;
  PerWorkerDiagnostics& operator=(PerWorkerDiagnostics&&) = delete;

  mem::Arena arena;
};

// Parses and desugars one admitted file, reporting whether it did.
//
// The arena answers an append it cannot hold with nothing rather than by
// trapping, naming neither the file nor the input, so a file it cannot
// hold is refused here instead - and it is what the arena has spent that
// is checked rather than how large the file is, since what a file costs
// is the identifiers it names.
//
// `bag` is the caller's: the run's own when one thread reads the input, and
// this file's when several do. `profiler` is where the phases are recorded,
// and null when they should not be.
bool parse_one(PipelineContext& ctx,
               ParsedFile& file,
               std::string_view bytes,
               diag::DiagBag& bag,
               debug::Profiler* profiler) {
  const u32 mark = bag.size();
  // What the file reports on its way out of a tree it could not finish
  // belongs to a shape that was never built, so those diagnostics are
  // dropped and the refusal is the one thing said about it.
  const auto refuse = [&] {
    bag.truncate(mark);
    const u32 index = bag.emit<i18n::Key::PipelineSpanArenaExhausted>(
        diag::Severity::Error, diag::Stage::Pipeline,
        DiagCode::SpanArenaExhausted, diag::Span{file.id, 0, 0},
        ctx.ast.spans.capacity());
    (void)index;
    return false;
  };
  if (ctx.ast.nearly_full() || ctx.ast.exhausted()) {
    return refuse();
  }
  lexer::Lexer lexer(bytes, file.id, bag);
  std::vector<lexer::Token> tokens;
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "tokenize", "frontend");
    lexer.tokenize(tokens);
  }
  parser::Parser parser(
      std::span<const lexer::Token>(tokens.data(), tokens.size()), bytes,
      file.id, ctx.ast, bag);
  base::Result<std::span<const ast::ItemIdx>, diag::Reported> parsed = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "parse", "frontend");
    return parser.parse();
  }();
  if (ctx.ast.exhausted()) {
    return refuse();
  }
  if (parsed.is_err()) {
    return false;
  }
  file.items = std::move(parsed).unwrap();
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler, "desugar", "frontend");
    parser::desugar_shadowing(file.items, ctx.ast, bag);
  }
  if (ctx.ast.exhausted()) {
    return refuse();
  }
  return true;
}

}  // namespace

base::Result<ParsedFiles, diag::Reported> parse_files(
    PipelineContext& ctx,
    std::span<const source::FileId> files) {
  ParsedFiles parsed;
  parsed.by_id.assign(ctx.sources.file_count(), ParsedFiles::NOT_PARSED);
  parsed.files.reserve(files.size());

  // Every file is admitted before any is parsed: its name is what a
  // diagnostic calls it, and its path is what the module tree reports a
  // stray file by. An id the source manager never minted is a caller's
  // bug rather than an input, so it ends the run here instead of
  // becoming a file some later stage trips over.
  std::vector<std::string_view> bytes;
  bytes.reserve(files.size());
  for (source::FileId id : files) {
    const std::optional<std::string_view> name = ctx.sources.name(id);
    const std::optional<std::string_view> content = ctx.sources.bytes(id);
    if (!name.has_value() || !content.has_value()) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineUnknownSourceFile>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::UnknownSourceFile);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    base::Result<path::Path, path::PathError> canonical =
        path::Path::from_native(*name);
    if (canonical.is_err()) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidSourcePath>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::InvalidSourcePath);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    parsed.by_id[id] = static_cast<u32>(parsed.files.size());
    parsed.files.push_back({id, std::move(canonical).unwrap(), {}});
    bytes.push_back(*content);
  }

  // What the loop below actually uses: one parser per file, and no more than
  // the thread count. The arena is told this and not the thread count, because
  // a lane it cuts is room no parser can reach: a single-file compile asked
  // for eight parsers would cut eight lanes and use one, and that one reads a
  // third of the table.
  const u32 workers =
      std::min(ctx.parse_jobs(), static_cast<u32>(parsed.files.size()));

  // Every append leaves room for the appends that may race with it, so
  // the slot count is what the arena is told before parsing begins.
  ctx.ast.set_parallel_slots(workers);

  // Several threads need a bag per file; one thread writes into the run's
  // own bag in the same order, so both paths report the same thing. The syntax
  // arena needs the same said about it: each thread appends to a lane of its
  // own, and which lane that is comes from the loop below rather than from a
  // count, because which file a thread takes is not knowable until it takes
  // one.
  if (workers < 2) {
    // The phases are named when one thread does the work: a region over
    // the whole parse would say nothing the phases do not.
    ast::set_current_lane(0);
    for (usize i = 0; i < parsed.files.size(); ++i) {
      if (!parse_one(ctx, parsed.files[i], bytes[i], ctx.bag, ctx.profiler)) {
        return base::make_err(diag::Reported{});
      }
    }
  } else {
    // One arena per worker rather than one per file, and one bag per file
    // built by the thread that reads it, over the arena of the worker that
    // thread is. Which thread reads a file is not knowable until it takes
    // one, so the bag cannot be built before the loop; the arena can, and
    // the worker count is what the loop below hands out. Merging still
    // reads the bags in file order, which is the order the one-thread path
    // reports in and is not the order the work finished in.
    std::vector<PerWorkerDiagnostics> per_worker(workers);
    std::vector<PerFileDiagnostics> per_file(parsed.files.size());
    {
      // The region is named when several threads do the work, and the
      // phases are not: under several threads a phase's duration is its
      // thread's rather than the run's, and a report that sums them reads
      // as though one file took as many threads as there are.
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "parse-files",
                                               "frontend");
      base::for_each(
          0, parsed.files.size(), workers, [&](usize at, usize worker) {
            ast::set_current_lane(static_cast<u32>(worker));
            per_file[at].bag = std::make_unique<diag::DiagBag>(
                per_worker[worker].arena, ctx.bag.language());
            const bool ok = parse_one(ctx, parsed.files[at], bytes[at],
                                      *per_file[at].bag, nullptr);
            per_file[at].ok.store(ok, std::memory_order_relaxed);
          });
    }

    // The lowest failing file ends the run; the header says why the
    // index and not the schedule decides.
    for (usize i = 0; i < per_file.size(); ++i) {
      if (!per_file[i].ok.load(std::memory_order_relaxed)) {
        for (usize j = 0; j <= i; ++j) {
          ctx.bag.merge(*per_file[j].bag);
        }
        return base::make_err(diag::Reported{});
      }
    }
    for (PerFileDiagnostics& into : per_file) {
      ctx.bag.merge(*into.bag);
    }
  }

  // Once, here, rather than after each file: see `verify_trees`. A file's
  // nodes are not a range of the arena once files are parsed on several
  // threads, so the whole arena is walked once every file is in it.
  if (parser::verify_trees(ctx.ast, ctx.bag).is_err()) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(std::move(parsed));
}

analyzer::ParsedModule parsed_module(const ParsedFiles& parsed,
                                     const analyzer::ModuleInput& input) {
  const ParsedFile* const file = parsed.find(input.id);
  DCHECK(file != nullptr);
  return analyzer::ParsedModule{input, file->path, file->items};
}

base::Result<analyzer::ModuleTree, diag::Reported> resolve_inputs(
    PipelineContext& ctx,
    source::FileId root,
    std::span<const analyzer::ModuleInput> modules,
    std::string_view package_name,
    std::span<const analyzer::ModuleInput> prelude,
    std::span<const analyzer::StdHint> std_hints,
    std::span<const analyzer::DependencyPackage> dependencies) {
  std::vector<source::FileId> files;
  files.reserve(modules.size() + prelude.size());
  for (const analyzer::ModuleInput& input : modules) {
    files.push_back(input.id);
  }
  for (const analyzer::ModuleInput& input : prelude) {
    files.push_back(input.id);
  }
  for (const analyzer::DependencyPackage& dependency : dependencies) {
    for (const analyzer::ParsedModule& input : dependency.modules) {
      files.push_back(input.input.id);
    }
  }
  base::Result<ParsedFiles, diag::Reported> parsed = parse_files(ctx, files);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const ParsedFiles sources = std::move(parsed).unwrap();
  const auto pair = [&](std::span<const analyzer::ModuleInput> inputs) {
    std::vector<analyzer::ParsedModule> paired;
    paired.reserve(inputs.size());
    for (const analyzer::ModuleInput& input : inputs) {
      paired.push_back(parsed_module(sources, input));
    }
    return paired;
  };
  std::vector<analyzer::ParsedModule> parsed_modules = pair(modules);
  std::vector<analyzer::ParsedModule> parsed_prelude = pair(prelude);
  std::vector<analyzer::DependencyPackage> parsed_dependencies;
  parsed_dependencies.reserve(dependencies.size());
  for (const analyzer::DependencyPackage& dependency : dependencies) {
    std::vector<analyzer::ParsedModule> parsed;
    parsed.reserve(dependency.modules.size());
    for (const analyzer::ParsedModule& input : dependency.modules) {
      parsed.push_back(parsed_module(sources, input.input));
    }
    parsed_dependencies.push_back(analyzer::DependencyPackage{
        dependency.identity, dependency.exports, parsed});
  }
  return analyzer::resolve_modules(root, parsed_modules, package_name, ctx.ast,
                                   ctx.bag, parsed_prelude, std_hints,
                                   parsed_dependencies);
}

}  // namespace pipeline
