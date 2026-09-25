// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/pipeline_context.h"

#include <string_view>

#include "config/build_config.h"
#include "fpag/mem/page_allocator.h"

namespace pipeline {

// Executable suffix for linked output (Windows needs .exe).
std::string_view exe_suffix() {
#if BUILD_FLAG(IS_OS_WIN)
  return ".exe";
#else
  return "";
#endif
}

PipelineContext::PipelineContext() : bag(arena), strings(mem::page_size()) {
  arena.reserve(1u << 20);
}

}  // namespace pipeline
