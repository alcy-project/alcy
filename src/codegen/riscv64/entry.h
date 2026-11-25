// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "codegen/riscv64/minst.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace codegen::riscv64 {

class RoData;

// The entry wrapper and the positions the backend patches once function
// and rodata offsets are known.
struct EntryBody {
  std::vector<MInst> insts;
  u32 call_at = 0;
  u32 labels = 0;
  std::optional<u32> message_at;
  std::optional<u32> err_index;
  i64 sret_bytes = 0;
  bool returns_enum = false;
};

[[nodiscard]] EntryBody build_entry(const ir::Function& function,
                                    const ir::Storage& storage,
                                    ir::PointerWidth width,
                                    RoData& rodata);

}  // namespace codegen::riscv64
