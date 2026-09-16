// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "ir/common.h"
#include "ir/type.h"

namespace ir {

struct BlockParam {
  Type type;
  RegisterIdx reg;
};

}  // namespace ir

