// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/type.h"

namespace ir {

struct FunctionMeta {
  Type return_type;
  TypeIdxRange param_types;

  str::StringPoolId name;
};

struct Function {
  FunctionMeta meta;

  BlockIdxRange blocks;
};

}  // namespace ir
