// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "codegen/x86_64/minst.h"
#include "codegen/x86_64/rodata.h"

// The runtime one object carries: the `alcy_*` bodies a program's own
// code calls, each written in the machine's own instructions at the C
// ABI the declaration spelled. The set is what a program can reach; a
// name outside it is a construct this backend has no answer for yet.
namespace codegen::x86_64::runtime {

// Whether the runtime has a body for this name.
[[nodiscard]] bool defines(std::string_view name);

// The body for one name. `defines` answers first; a name it refuses is
// an emitter bug, not an input the reader can fix.
[[nodiscard]] Body build(std::string_view name, RoData& rodata);

}  // namespace codegen::x86_64::runtime

