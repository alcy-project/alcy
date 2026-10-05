// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/target.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/build/build_config.h"
#include "ir/type.h"

namespace codegen {

bool Target::is_windows() const {
  return triple.find("windows") != std::string::npos;
}

bool Target::is_wasm() const {
  return triple.rfind("wasm", 0) == 0;
}

std::string host_triple() {
  // Spelled from the compiler's own platform macros rather than asked of
  // LLVM, because a build with no LLVM still answers this. The values are
  // what llvm::sys::getDefaultTargetTriple names the same hosts, minus
  // the OS version suffix a Darwin triple can carry: the LLVM backend
  // accepts a versionless triple and derives the same machine from it.
#if defined(__EMSCRIPTEN__)
  return "wasm32-unknown-emscripten";
#elif FPAG_BUILD_FLAG(IS_OS_WIN)
#if FPAG_BUILD_FLAG(IS_ARCH_ARM64)
  return "aarch64-pc-windows-msvc";
#else
  return "x86_64-pc-windows-msvc";
#endif
#elif FPAG_BUILD_FLAG(IS_OS_MAC)
#if FPAG_BUILD_FLAG(IS_ARCH_ARM64)
  return "arm64-apple-darwin";
#else
  return "x86_64-apple-darwin";
#endif
#elif FPAG_BUILD_FLAG(IS_OS_LINUX)
#if FPAG_BUILD_FLAG(IS_ARCH_X86_64)
  return "x86_64-unknown-linux-gnu";
#elif FPAG_BUILD_FLAG(IS_ARCH_ARM64)
  return "aarch64-unknown-linux-gnu";
#elif FPAG_BUILD_FLAG(IS_ARCH_RISCV64)
  return "riscv64-unknown-linux-gnu";
#else
#error "no host triple for this Linux architecture"
#endif
#else
#error "no host triple for this platform"
#endif
}

Target host_target() {
  Target target{host_triple(), ir::PointerWidth::W64};
#if defined(__EMSCRIPTEN__)
  // The one host this build runs on whose pointers are not 64-bit.
  target.width = ir::PointerWidth::W32;
#endif
  return target;
}

std::vector<std::string> target_names() {
  return {"host", "wasm32-unknown-emscripten"};
}

std::optional<Target> target_from_name(std::string_view name) {
  if (name == "host") {
    return host_target();
  }
  if (name == "wasm32-unknown-emscripten") {
    return Target{std::string(name), ir::PointerWidth::W32};
  }
  return std::nullopt;
}

}  // namespace codegen
