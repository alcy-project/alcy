// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/runtime_stage.h"

#include <string_view>

#include "app/embedded_runtime.h"
#include "fpag/base/numeric.h"
#include "fpag/io/temp_dir.h"

namespace app {

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

}  // namespace app
