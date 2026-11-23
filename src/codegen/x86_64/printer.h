// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <vector>

#include "codegen/backend.h"
#include "codegen/object/elf64.h"
#include "codegen/x86_64/minst.h"

namespace codegen::x86_64 {

// One function's allocated body, with the name its bytes carry and any
// aliases another symbol gives the same address.
struct PrintedBody {
  std::string symbol;
  Body body;
  object::SymbolBind bind = object::SymbolBind::Local;
  std::vector<std::string> aliases;
  u64 offset = 0;
  u64 size = 0;
};

// Renders the whole object as one assembly source: the sections and
// symbols it carries, and each body's instructions in `syntax`. The text
// assembles to the same program, not to the same object bytes.
[[nodiscard]] std::string print_assembly(
    const object::Object& object,
    std::span<const PrintedBody> bodies,
    codegen::AsmSyntax syntax = codegen::AsmSyntax::Att);

}  // namespace codegen::x86_64
