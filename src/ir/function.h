// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/str/string_pool_id.h"
#include "ir/common.h"

namespace ir {

struct FunctionMeta {
  TypeIdx return_type;
  TypeIdxRange param_types;

  str::StringPoolId name;
};

struct Function {
  FunctionMeta meta;

  BlockIdxRange blocks;
};

}  // namespace ir
