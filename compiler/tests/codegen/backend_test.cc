// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/backend.h"

#include <optional>
#include <string>

#include "codegen/target.h"
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
    CHECK(!host->is_wasm());
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
#if ALCY_BACKEND_DIRECT_WASM
  CHECK(default_backend(wasm) == Backend::DirectWasm);
#elif ALCY_BACKEND_LLVM
  // This build has no wasm emitter, so LLVM answers the wasm target the
  // same way it answers the host.
  CHECK(default_backend(wasm) == Backend::Llvm);
#endif
}

}  // namespace codegen
