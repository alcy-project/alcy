// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/pipeline_context.h"

#include <string>
#include <string_view>

#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/mem/page_allocator.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "path/path.h"

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
          diag::Severity::Error, PIPELINE_IO_ERROR, prefix);
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }
  if (!io::create_directory(std::string(path))) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreateDirectory>(
        diag::Severity::Error, PIPELINE_IO_ERROR, path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

PipelineContext::PipelineContext(i18n::Language language)
    : bag(arena, language), strings(mem::page_size()) {
  arena.reserve(1u << 20);
}

}  // namespace pipeline
