// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/runtime_stage.h"

#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "pipeline/embedded_runtime.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

namespace {

std::string_view as_view(const unsigned char* data, u64 len) {
  return std::string_view(reinterpret_cast<const char*>(data), len);
}

}  // namespace

base::Result<void, diag::Reported> stage_runtime(io::TempDir& dir,
                                                 diag::DiagBag& bag) {
  if (!dir.write_file(runtime_header_name(),
                      as_view(ALCY_RUNTIME_HEADER, ALCY_RUNTIME_HEADER_LEN)) ||
      !dir.write_file(runtime_source_name(),
                      as_view(ALCY_RUNTIME_SOURCE, ALCY_RUNTIME_SOURCE_LEN))) {
    const u32 index = bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                               "cannot stage the runtime");
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

std::string_view runtime_header_name() {
  return "alcy_runtime.h";
}

std::string_view runtime_source_name() {
  return "alcy_runtime.c";
}

}  // namespace pipeline
