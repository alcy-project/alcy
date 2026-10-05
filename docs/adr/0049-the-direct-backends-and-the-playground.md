# ADR-0049: The direct backends, and the playground built on one

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-06

## Context

Two uses want machine code without LLVM's cost, and neither is served by the
current build.

The first is a browser playground: a page takes source text, compiles it, and
runs the result. That build wants a small module, a fast start, and no
toolchain behind it. Today's wasm build is the opposite of that: `build.py
--target=all --target-os=emscripten` compiles the CLI, the tests, and the
whole LLVM fork's wasm install into the artifact, and `package.py` ships
`alcy.js`, `alcy.wasm`, `libalcy.a`, and the test binaries. LLVM is in that
build for optimization and linking, and the playground needs neither.

The second is the debug profile on the host. `--release` is where LLVM earns
its keep; a debug build runs no optimization pass at all, yet it still builds
an `llvm::Module`, runs the emitter, and goes through SelectionDAG to write an
object. The IR `lowering` produces is already low-level -- SSA registers,
explicit `Alloca`/`Load`/`Store`, blocks, calls -- so a small emitter can turn
it into an object directly, and the embedded lld the compiler already links
through does the linking in-process.

`compiler/codegen/` is the reserved place for this. Its README states the
contract: consume verified IR, return `base::Result`, report through the bag.
What it does not state is the shape of a second backend beside `codegen_llvm`,
the language features a first version must cover, or what the playground
exposes. This record fixes those.

## Decision

### A backend seam in `codegen/`

`codegen/` becomes the abstraction and `codegen_llvm/` one implementation of
it. The pieces:

- `codegen/target.{h,cc}`: `Target` (triple and pointer width) and
  `host_triple()` move out of `codegen_llvm`, so `PipelineContext` no longer
  depends on an LLVM module to know what it builds for.
- `codegen/backend.h`: a `Backend` enum, an `EmitRequest` carrying verified
  storage, the string interner, the bag, `Target`, `emit_entry`, `optimize`,
  and the profiler, and one entry point per output kind. Implementations are
  selected by a `switch` in one dispatch file. Static dispatch with concepts,
  not virtuals: the project is `-fno-rtti` and `-fno-exceptions`, and a
  backend is chosen once per build, not per call.
- `EmitMode` gains `Wasm`. The direct wasm backend answers only that mode and
  `--emit=llvm-ir`/`--emit=llvm-bitcode` stay LLVM-only; asking another
  backend for them is a diagnostic, not a fallback.
- GN selects implementations with `alcy_backends`, a list defaulting to
  `["llvm"]`. A build without `llvm` in the list does not depend on
  `//third_party/llvm`, so `setup_llvm` never runs and nothing is downloaded.
- There is no LIR. The IR is already the low-level form, and a second one
  would be a converter and a second verifier for no consumer that does not
  exist yet. What the backends share -- frame layout, value-to-slot
  assignment, runtime declarations -- lives in `codegen/` as implementation
  detail, not as pipeline-visible IR.

### The direct backends

`codegen/x86/` and `codegen/wasm/` are alcy's own emitters, named `direct` in
prose and in the code, and selected as `--backend=direct-wasm` and eventually
`--backend=direct-x86`. The family name is `direct` because the choice a
reader makes is between going through LLVM and generating code directly;
`native` would be wrong for a compiler running on wasm that emits wasm, and
`alcy` is the name of the language and the compiler already.

The emitters allocate every value to a stack slot and keep no register state
across instructions beyond a fixed scratch set. There is no register
allocator, and there is no optimizer: `--release` with a direct backend is an
error rather than a silently unoptimized binary. Link-visible functions --
anything another object may call -- follow the target's ABI where one exists
(System V for x86-64), because an object from this backend has to be
replaceable by one from the LLVM backend and vice versa; compiler-internal
helpers may use a private convention. A feature the emitter does not cover is
a diagnostic naming the construct and its source span, never a wrong
encoding.

The wasm backend emits a final module, not a relocatable object, and its
control flow is a dispatch loop: every IR block becomes a case of a
`br_table` selected by a state local. That handles any CFG, reducible or not,
without a relooper, and the playground is not a place where the loop's cost
matters.

### The playground

The playground build is `alcy_backends = ["direct-wasm"]` with no LLVM, and
it exposes a C ABI rather than a CLI:

```c
typedef struct {
  u8* wasm;            usize wasm_len;
  u8* diagnostics;     usize diagnostics_len;  // JSON
  u32 file_count;      usize module_count;
  usize function_count;
  i32 ok;
} AlcyResult;

i32 alcy_check(const u8* src, usize len, AlcyResult* out);
i32 alcy_compile(const u8* src, usize len, AlcyResult* out);
void alcy_release(AlcyResult* out);
```

`alcy_check` answers what `alcy check` answers and fills the counts;
`alcy_compile` fills the wasm bytes. Diagnostics are the `--json` envelope's
diagnostic array, so the page and the CLI cannot disagree about a message.
The bytes are owned by the caller until `alcy_release`.

The emitted module speaks WASI preview1: it exports `_start` and imports
`fd_write` and `proc_exit` from `wasi_snapshot_preview1`. The page supplies a
small shim, instantiates, and calls `_start` -- "run" is compile followed by
that call. Standardizing on WASI rather than a private import keeps the
playground's output runnable by `node`, `wasmtime`, and the full compiler's
output alike.

The second wasm build -- the portable full compiler -- keeps LLVM and embeds
the wasm lld driver, so it can optimize and link in a wasm host. It is the
same pipeline with `--backend=llvm`, and it lands after the playground.

## Consequences

The playground ships without LLVM: a module measured in megabytes rather than
tens, built and instantiated quickly enough for a button press. The host
debug profile gains a path that skips module construction and SelectionDAG,
which is where its codegen time goes; [ADR-0046](0046-the-frontend-cost-is-per-function.md)
already showed the frontend near its floor, so this is the remaining stage
worth measuring.

The costs are real. A second emitter for each architecture has to be written,
tested against the LLVM backend's output, and kept current with the IR. The
`alcy_backends` build matrix doubles the configurations CI must cover. The
dispatch loop is slower than structured control flow, and the stack-machine
code is slower than anything LLVM emits; that is accepted for the playground
and the debug profile, and is why `--release` refuses a direct backend.
Embedding the wasm lld driver and its driver-side inputs is its own work,
because today's embedded linker is the ELF driver with Linux startup inputs.

Out of scope, and recorded as such: cross-target builds (`--target`), the
in-memory lld interface (objects keep going through the temporary file), a
persistent `backend` setting in manifests or toolchain files (that waits for
the profile concept), and the playground's site itself.

Milestones:

- **M0**: the seam. Neutral `Target`, the dispatch, `EmitMode::Wasm`,
  `alcy_backends`, `--backend`, and a build with no LLVM that passes `check`.
- **M1**: `direct-wasm`. Module writer, dispatch loop, WASI runtime, bump
  allocator, unit tests, and the same programs run under `node` as under
  LLVM.
- **M2**: the playground. `alcy_check`/`alcy_compile`/`alcy_release`, the
  Emscripten target, the JS shim, a CI job.
- **M3**: the portable full compiler. The wasm lld driver embedded, its
  inputs supplied without a host toolchain, the same API surface.
- **M4**: `direct-x86`, then whatever the measurements say.
