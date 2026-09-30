# ADR-0023: The Program Runtime Is Built as IR in the Program's Module

- Subject: the compiler
- Status: Accepted
- Date: 2026-09-30

## Context

Every executable build staged `runtime/alcy_runtime.c` — a C source
embedded in the compiler — into a temporary directory and spawned the
system C compiler on it, once per build, as the link driver's second
input. The runtime is a fixed set of seven functions over libc
(`write`/`_write`, `posix_memalign`/`_aligned_malloc`, `abort`,
`free`), so the work was identical for every program and its result was
thrown away with the temporary directory. On the measurement machine a
hello-world link ran 60.1 ms, of which that compilation was 30 ms, the
driver's own startup 18 ms, and the linker 12 ms.

The same functions are what the program's module already declares:
lowering emits `declare ptr @alcy_alloc(i64, i64)` and its siblings and
calls them, so the runtime's whole job is to define what the module
asks for.

## Decision

`codegen_llvm::add_runtime_definitions(module, width)` defines the
runtime in the program's module as LLVM IR. `EmittedModule::build`
calls it after the program is emitted — so the definitions land in the
declarations the program's call sites already hold, with their
signatures `DCHECK`ed against them — and before `optimize_module`, so
the runtime is optimized with the program rather than compiled at
whatever the driver's default level is.

The functions and their bindings are the C runtime's, unchanged:
`write_all` retries a short write and gives up when the descriptor
stops accepting bytes, a null message is treated as empty and the
length drops with it, `alcy_panic` writes to stderr and aborts,
`alcy_alloc` rejects an alignment that is zero or not a power of two
and returns null, POSIX promotes alignments below pointer size before
`posix_memalign`, Windows calls `_aligned_malloc`, and a zero-size
request still yields a distinct freeable pointer. `write_all` is
`internal` and the entry points are `linkonce_odr`: every object alcy
writes carries the runtime, so a definition the linker may discard is
what lets two of them meet in one image, and it is what lets the
optimizer drop an entry point no program calls.

The pointer width comes from the target (`TARGET_WIDTH`), not from a
DataLayout: the IR uses opaque pointers and fixes the element types it
addresses, so the runtime is built before the triple is set, on the
module the emitter just filled.

Every artifact alcy writes carries the runtime — the object, the
textual IR, and the bitcode are one module — and the executable's link
line loses an input: `{driver, object, ...args, -o, exe}`. Staging,
embedding, and the C sources are gone, along with `link_executable`'s
`runtime_path` and the runtime check that compiled the C file.

**Staging the runtime as bitcode instead was rejected.** The pinned
LLVM ships libraries and headers, not a compiler, so a checked-in or
generated `.bc` would come from the host C compiler (22.1.8 against the
fork's 23.1.2 here), and every build would depend on the version that
made it — the reproducibility the pinned fork exists to provide. A
bitcode round trip through `llvm::Linker` was not needed once the IR is
built in-process, and neither was `llvm::lto::LTO`: the merge is a
lookup in the module, not a link of two modules.

## Consequences

- A build spawns one less process, and its link phase halves on the
  measurement machine: 60.1 ms → 29.2 ms, with the hello-world total at
  114 ms → 85.2 ms. No C compiler is needed to produce an executable.
- The runtime is optimized with the program. Its internal helper becomes
  an `internal` function the optimizer gives a private calling convention
  and inlines where it pays, and the module's functions get attributes
  inferred from their bodies.
- Two objects no longer collide on the runtime. Linking two of them
  relocatably reported all six entry points as duplicate symbols before
  and none after. What still collides is alcy's own code: a program and
  the standard library it compiled in are emitted with external linkage
  ([`docs/adr/0011-symbol-mangling.md`](0011-symbol-mangling.md)), so two objects sharing a std
  function collide on that one instead. Making a multi-object image work
  is lib packages' question, and this change takes the runtime off the
  list rather than the whole of it.
- A discardable entry point is also one the optimizer drops, which the C
  runtime's object could not do. A release hello-world carries no
  `alcy_*` symbol at all — the reachable path inlined into the program,
  the rest removed — and its object is 12% smaller for it.
- Emitted IR and bitcode show the runtime. That is the module, not a
  defect: what an executable links is what `--emit=object` writes, and
  the sanitized exe harness compiles that one module with clang, so the
  runtime is instrumented too.
- `alcy_alloc` keeps one alloca in every module, because
  `posix_memalign` writes its result through a pointer and no
  optimization can promote an address that escapes. Tests that read
  "no alloca after O3" as "mem2reg ran" must count the program's own
  spills, which are told apart by element type.
- The Windows branch is exercised where it matters: the `windows-x64`
  jobs compile, link, and run the exe acceptance cases, so `_write`,
  `_aligned_malloc`, and `_aligned_free` all run. The sanitized pass over
  the same cases is local to `check.sh`.
