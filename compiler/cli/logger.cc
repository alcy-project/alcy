// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/logger.h"

#include <string_view>

#include "debug/dcheck.h"
#include "fpag/io/io_util.h"

namespace cli {

void Logger::block(std::string_view text) const {
  DCHECK(is_block(text));
  write_(ctx_, text);
}

void FdSink::write(void* ctx, std::string_view block) {
  io::write(static_cast<const FdSink*>(ctx)->fd, block.data(), block.size());
}

}  // namespace cli
