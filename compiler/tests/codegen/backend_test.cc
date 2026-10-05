// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/backend.h"

#include <optional>
#include <string>

#include "codegen/target.h"
#include "config/build_config.h"
#include "doctest/doctest.h"
#include "ir/type.h"

namespace codegen {

TEST_CASE("Backend names round-trip") {
  for (const Backend backend : {Backend::Llvm, Backend::DirectWasm}) {
    const std::optional<Backend> found =
        backend_from_name(backend_name(backend));
    CHECK(found.has_value());
    if (found.has_value()) {
      CHECK(*found == backend);
    }
  }
  CHECK(!backend_from_name("native").has_value());
  CHECK(backend_name(Backend::None) == "none");
  CHECK(!backend_available(Backend::None));
}

TEST_CASE("Target names resolve") {
  const std::optional<Target> host = target_from_name("host");
  CHECK(host.has_value());
  if (host.has_value()) {
    CHECK(host->triple == host_triple());
    // The Emscripten build runs on the wasm machine it emits for; every
    // other host is not one.
#if BUILD_FLAG(IS_OS_ASMJS)
    CHECK(host->is_wasm());
#else
    CHECK(!host->is_wasm());
#endif
  }

  const std::optional<Target> wasm =
      target_from_name("wasm32-unknown-emscripten");
  CHECK(wasm.has_value());
  if (wasm.has_value()) {
    CHECK(wasm->is_wasm());
    CHECK(wasm->width == ir::PointerWidth::W32);
  }

  CHECK(!target_from_name("x86_64-unknown-linux-gnu").has_value());
  for (const std::string& name : target_names()) {
    CHECK(target_from_name(name).has_value());
  }
}

TEST_CASE("The default backend follows the target") {
  const Target wasm{"wasm32-unknown-emscripten", ir::PointerWidth::W32};
#if ALCY_BACKEND_LLVM
  CHECK(default_backend(host_target()) == Backend::Llvm);
#endif
  CHECK(default_backend(wasm) == Backend::DirectWasm);
}

}  // namespace codegen
