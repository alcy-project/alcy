// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/runtime_stage.h"

#include <string_view>

#include "cli/embedded_runtime.h"
#include "fpag/base/numeric.h"
#include "fpag/io/temp_dir.h"

namespace cli {

namespace {

std::string_view as_view(const unsigned char* data, u64 len) {
  return std::string_view(reinterpret_cast<const char*>(data), len);
}

}  // namespace

bool stage_runtime(io::TempDir& dir) {
  if (!dir.write_file(runtime_header_name(),
                      as_view(kAlcyRuntimeHeader, kAlcyRuntimeHeader_len))) {
    return false;
  }
  return dir.write_file(runtime_source_name(),
                        as_view(kAlcyRuntimeSource, kAlcyRuntimeSource_len));
}

std::string_view runtime_header_name() {
  return "alcy_runtime.h";
}

std::string_view runtime_source_name() {
  return "alcy_runtime.c";
}

}  // namespace cli
