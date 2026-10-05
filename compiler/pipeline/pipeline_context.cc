// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/pipeline_context.h"

#include <string>
#include <string_view>

#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/build/build_flag.h"
#include "fpag/io/io_util.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"

namespace pipeline {

std::string_view exe_suffix() {
#if BUILD_FLAG(IS_OS_WIN)
  return ".exe";
#else
  return "";
#endif
}

base::Result<void, diag::Reported> ensure_directories(PipelineContext& ctx,
                                                      std::string_view path) {
  if (path.empty() || path == "." || path == "/") {
    return base::make_ok();
  }
  for (usize i = 1; i < path.size(); ++i) {
    if (path[i] != path::DEFAULT_PATH_SEPARATOR) {
      continue;
    }
    const std::string_view prefix = path.substr(0, i);
    // "C:" is a drive root, not a directory to create.
    if (prefix.ends_with(':')) {
      continue;
    }
    if (!io::create_directory(std::string(prefix))) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreateDirectory>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          prefix);
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }
  if (!io::create_directory(std::string(path))) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreateDirectory>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

// What the diagnostic arena is reserved. Every message a run reports is
// copied in, so the need follows how much a compiler has to say rather
// than how much it read: a mebibyte holds a few thousand messages, and a
// program that produces that many is already past the point where a
// reader wants them. The reservation is address space, and pages are
// committed as messages arrive.
#if BUILD_FLAG(IS_ARCH_64_BITS)
constexpr usize DIAGNOSTIC_CAPACITY = 16ull << 20;
#else
constexpr usize DIAGNOSTIC_CAPACITY = 2ull << 20;
#endif

PipelineContext::PipelineContext(i18n::Language language,
                                 usize span_capacity,
                                 usize name_capacity)
    : ast(span_capacity), bag(arena, language), strings(name_capacity) {
  arena.reserve(DIAGNOSTIC_CAPACITY);
}

u32 PipelineContext::parse_jobs() const {
  // Zero means the command line did not ask for threads, and one thread is
  // the answer: reading the source is a small part of a run, so spreading it
  // costs the passes that read what it built more than it saves.
#if FPAG_BUILD_FLAG(IS_OS_ASMJS)
  // A target without threads has nothing to spread the work over, and the
  // count a caller asked for cannot make it appear. One is the answer here
  // rather than at the loop because more than one is read as a promise: the
  // syntax arena cuts a lane of itself for each parser it is told to expect,
  // and a lane no parser takes is room the one parser that does the work
  // cannot reach. A package whose arena is small -- the 32-bit targets
  // reserve an eighth of what a 64-bit one does -- runs out of it while a
  // third of it is sitting in lanes.
  (void)jobs;
  return 1;
#else
  return jobs == 0 ? 1 : jobs;
#endif
}

}  // namespace pipeline
