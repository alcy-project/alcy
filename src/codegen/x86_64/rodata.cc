// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/rodata.h"

#include <string>
#include <string_view>
#include <utility>

#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

const std::string& RoData::intern(const std::string_view bytes) {
  std::string key(bytes);
  const auto found = index_.find(key);
  if (found != index_.end()) {
    return names_[found->second];
  }
  const u32 at = static_cast<u32>(names_.size());
  names_.push_back("alcy.str." + std::to_string(at));
  bytes_.emplace_back(bytes);
  index_.emplace(std::move(key), at);
  return names_.back();
}

}  // namespace codegen::x86_64
