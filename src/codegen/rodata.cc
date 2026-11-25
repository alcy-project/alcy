// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/rodata.h"

#include <string>
#include <string_view>

namespace codegen {

u32 RoData::intern(const std::string_view bytes) {
  const auto found = index_.find(bytes);
  if (found != index_.end()) {
    return found->second;
  }
  const u32 at = static_cast<u32>(names_.size());
  names_.push_back("alcy.str." + std::to_string(at));
  bytes_.emplace_back(bytes);
  index_.emplace(bytes_.back(), at);
  return at;
}

}  // namespace codegen
