// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/backend.h"

#include <optional>
#include <string_view>

#include "codegen/target.h"

namespace codegen {

std::string_view backend_name(Backend backend) {
  switch (backend) {
    case Backend::None: return "none";
    case Backend::Llvm: return "llvm";
    case Backend::DirectWasm: return "direct-wasm";
  }
  return "none";
}

std::optional<Backend> backend_from_name(std::string_view name) {
  if (name == "llvm") {
    return Backend::Llvm;
  }
  if (name == "direct-wasm") {
    return Backend::DirectWasm;
  }
  return std::nullopt;
}

bool backend_available(Backend backend) {
  switch (backend) {
    case Backend::None: return false;
    case Backend::Llvm:
#if ALCY_BACKEND_LLVM
      return true;
#else
      return false;
#endif
    case Backend::DirectWasm:
#if ALCY_BACKEND_DIRECT_WASM
      return true;
#else
      return false;
#endif
  }
  return false;
}

Backend default_backend(const Target& target) {
  // A wasm target is what the direct backend exists for: it produces the
  // module the target names, where LLVM would produce an object that
  // still needs a wasm linker this build does not carry.
  if (target.is_wasm() && backend_available(Backend::DirectWasm)) {
    return Backend::DirectWasm;
  }
  if (backend_available(Backend::Llvm)) {
    return Backend::Llvm;
  }
  if (backend_available(Backend::DirectWasm)) {
    return Backend::DirectWasm;
  }
  return Backend::None;
}

Backend default_backend() {
  return default_backend(host_target());
}

}  // namespace codegen
