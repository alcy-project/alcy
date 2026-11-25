// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "codegen/object/elf64.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

// One entry in a backtrace table: an address, a byte count for functions,
// and a name the runtime prints. Sites carry only the address.
struct BacktraceEntry {
  i64 offset = 0;
  u64 size = 0;
  std::string text;
};

struct BacktraceTables {
  std::vector<BacktraceEntry> functions;
  std::vector<BacktraceEntry> sites;
  bool enabled = false;

  [[nodiscard]] u64 functions_size() const;
  [[nodiscard]] u64 sites_size() const;
};

// Append both tables to the text bytes and name them in the object.
void emit_tables(const BacktraceTables& tables,
                 u64 functions_start,
                 u64 sites_start,
                 std::vector<u8>& bytes,
                 object::Object& object);

}  // namespace codegen::riscv64
