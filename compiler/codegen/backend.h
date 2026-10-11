// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <concepts>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/target.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"

namespace codegen {

// Which emitter produces the machine code. `None` is a build that carries
// no backend at all: `check` still works, and a build command reports it
// rather than producing nothing.
enum class Backend : u8 { None, Llvm, DirectWasm };

// What a backend is asked to write. `Object` is a relocatable object for
// the linker; `Module` is a final module that needs no link (wasm today);
// `Text` and `Bitcode` are the two readable LLVM forms, which only a
// backend with a module to print can answer.
enum class OutputKind : u8 { Object, Module, Text, Bitcode };

// Why an emission failed. Anything finer lives in the bag the request
// carries; this is what a caller switches on to choose its own message.
enum class EmitError : u8 {
  UnknownTarget,
  CannotEmit,
  CannotOptimize,
  Unsupported,
  NoOptimizer,
};

// One emission request. The storage is moved in: the emitter consumes the
// IR, and the proof it carries is what lets the backends trust it.
struct EmitRequest {
  ir::VerifiedStorage storage;
  // Source spans parallel to storage instructions, for a backend that
  // reports a construct it cannot encode at the place it was written.
  std::span<const diag::Span> instr_spans;
  // Rendered `file:line:column` spellings parallel to instr_spans, for a
  // backend that names call sites. Empty unless a backtrace asked for
  // them; entries without a site carry the empty name.
  std::span<const str::StringPoolId> instr_site_names;
  ir::SymbolTable* strings = nullptr;
  diag::DiagBag* bag = nullptr;
  Target target;
  // Whether the module owns the program entry: a wrapper around `main`
  // that a binary needs and a library does not.
  bool emit_entry = false;
  // Whether the entry is `_start` and the link skips the C runtime
  // (ADR-0052); only meaningful with `emit_entry`.
  bool freestanding = false;
  // Whether a panic walks the frame chain and names the frames it can
  // (ADR-0070). Only the LLVM backend keeps frame pointers and emits the
  // table the walk reads; the direct wasm backend writes the message and
  // its site and no frames.
  bool backtrace = false;
  // Whether to run the middle end. A backend without one answers
  // `NoOptimizer` rather than emitting unoptimized code under a release
  // command.
  bool optimize = false;
  // Whether the source operations check overflow: a debug build
  // aborts on `+ - * / %` and shift overflow, a release one wraps them
  // (ADR-0062). The policy is a property of the build, not of the IR,
  // so the same verified IR serves every profile; a backend that does
  // not carry the checks yet ignores this.
  bool overflow_checks = false;
  OutputKind kind = OutputKind::Object;
  debug::Profiler* profiler = nullptr;
  // How many objects code generation may be split into, for a backend that
  // can write more than one. The count is a function of the program, never
  // of the machine running the build: what a run writes must not depend on
  // how many threads read it.
  u32 parts = 1;
  // How many threads the split may use. A backend that ignores `parts`
  // ignores this too.
  u32 workers = 1;
};

// A backend implementation. `emit` owns the request: the storage moves
// into whatever the emitter builds from it.
template <class Impl>
concept BackendImpl = requires {
  { Impl::ID } -> std::convertible_to<Backend>;
  {
    Impl::supports(std::declval<OutputKind>(), std::declval<const Target&>())
  } -> std::same_as<bool>;
  {
    Impl::emit(std::declval<EmitRequest>())
  } -> std::same_as<base::Result<std::vector<u8>, EmitError>>;
};

// Calls one implementation. The concept is the contract; this is the one
// place a caller spells the implementation type.
template <BackendImpl Impl>
base::Result<std::vector<u8>, EmitError> emit_with(EmitRequest request) {
  return Impl::emit(std::move(request));
}

// The same, answering with every artifact the request produced. A backend
// that can split code generation across objects answers `emit_artifacts`;
// one that always writes a single artifact is answered by `emit` and the
// blob becomes the one artifact.
template <class Impl>
base::Result<std::vector<std::vector<u8>>, EmitError> emit_artifacts_with(
    EmitRequest request) {
  if constexpr (requires(EmitRequest one) {
                  { Impl::emit_artifacts(std::move(one)) };
                }) {
    return Impl::emit_artifacts(std::move(request));
  } else {
    base::Result<std::vector<u8>, EmitError> single =
        Impl::emit(std::move(request));
    if (single.is_err()) {
      return base::make_err(std::move(single).unwrap_err());
    }
    std::vector<std::vector<u8>> artifacts;
    artifacts.push_back(std::move(single).unwrap());
    return base::make_ok(std::move(artifacts));
  }
}

// The name a `--backend` spelling uses, and the reverse. `None` names no
// spelling; an unknown name answers nothing.
[[nodiscard]] std::string_view backend_name(Backend backend);
[[nodiscard]] std::optional<Backend> backend_from_name(std::string_view name);

// Whether this build carries the implementation. The answer comes from
// the build configuration, not from a runtime registry: a backend that
// was not compiled in has no code to run.
[[nodiscard]] bool backend_available(Backend backend);

// The backend a command uses when it names none: the first implementation
// that can write for `target` - a wasm target prefers the direct wasm
// backend, everything else LLVM - and None when the build carries none.
[[nodiscard]] Backend default_backend(const Target& target);

// The same for the host machine.
[[nodiscard]] Backend default_backend();

}  // namespace codegen
