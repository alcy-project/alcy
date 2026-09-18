# Architecture

This document describes the intended architecture of the alcy compiler at
MVP: a working compiler that translates alcy source code into executables.
Implementation details may differ while the project is under development;
this document is updated when the design or its architectural contracts
change.

This document covers *what* the compiler is and *why* it is structured this
way. For day-to-day build, test, lint, formatting, and contribution rules,
see [CONTRIBUTING.md](CONTRIBUTING.md). Detailed subsystem documentation
belongs in `docs/`.

## Core design principles

When implementing or modifying any component in alcy, preserve the following
principles:

- **Clear Responsibilities & Module Boundaries**: Each module should have a
  small, well-defined responsibility and a clear dependency direction.
  Split responsibilities when doing so makes boundaries clearer, and avoid
  coupling modules merely for convenience.

- **Explicit Behavior**: Important behavior should be visible at the call
  site. Control flow, ownership, mutation, and non-trivial work should not
  be hidden behind implicit conversions, operator overloading, inheritance,
  callbacks, or other language machinery.

- **Local Reasoning**: Prefer code whose behavior can be understood from its
  surrounding context without tracing framework machinery, hidden state, or
  distant registration. Prefer concrete data and explicit control flow over
  clever abstractions.

- **Predictable Cost**: Allocation, synchronization, I/O, and other
  potentially expensive work should be explicit where practical.
  Abstractions must not introduce runtime costs that are surprising relative
  to their apparent semantics.

- **Zero-Allocation Hot Paths**: The core compilation loop (lexing, parsing,
  IR transformation, and analysis) must avoid heap allocation on hot paths.

- **Pragmatic Simplicity**: Build only what the current design requires.
  Prefer the simplest design that preserves architectural boundaries and
  makes invariants obvious. Do not introduce abstractions solely to avoid
  small amounts of duplication when the abstraction would obscure behavior
  or couple otherwise independent components.

- **Production-Ready Quality**: Core compiler code must maintain explicit
  invariants, deterministic behavior, robust error handling, and tests
  appropriate to the component. Prototype shortcuts must not become
  architectural dependencies.

- **No Runtime Polymorphism**: Compiler code must not use virtual functions,
  virtual inheritance, or RTTI. Prefer concrete types, enums, tagged unions,
  and explicit dispatch where behavior varies by case.

## Overview

alcy is a statically-typed programming language with ownership-based memory
management. The reference compiler is written in C++20 and lowers alcy
source code to LLVM IR, relying on LLVM for optimization and object code
generation.

The MVP scope is a single-threaded batch compiler: given source files, it
produces an executable.

In scope:

- Lexing and parsing.
- AST construction.
- IR construction.
- Name resolution, type checking, and ownership checking.
- A linear single-pass compilation pipeline.
- LLVM IR emission and object-file generation.

Deliberately out of scope for MVP:

- Parallel or incremental compilation.
- Language-server functionality.
- Build-system dependency tracking.
- A completed native (non-LLVM) backend.

## Architectural shape

Compilation is organized as a linear sequence of stages. Each stage consumes
explicitly owned or borrowed data produced by the previous stage. Stages do
not communicate through shared mutable global state.

The dependency structure should follow the same direction as the pipeline:
higher-level orchestration may depend on lower-level data structures and
utilities, but lower-level modules must not depend on higher-level policy or
orchestration code.

Where behavior varies by case, prefer explicit data-driven dispatch over
runtime polymorphism.

## Repository layout

- `src/` — the compiler. One directory per module, each with a `BUILD.gn`.
- `build/` — GN build configuration: toolchains (`build/toolchains/`),
  compiler flags (`build/config/`), and helper scripts (`build/scripts/`).
- `third_party/` — vendored dependencies as submodules (`llvm`, `fpag`,
  `fmt`, `doctest`, `xxhash`), each wrapped with a `BUILD.gn`.
- `docs/` — detailed documentation and architecture decision records
  (`docs/adr/`).

## Compiler modules

Compilation proceeds as a linear pipeline. There is no shared mutable global
state between stages beyond the data explicitly passed along.

| Module                 | Role                                                                                                                                   | Allocation contract                                                               |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------| --------------------------------------------------------------------------------- |
| `app`                  | Driver: argument parsing, initialization, and pipeline orchestration.                                                                  | Standard allocation; setup-time work only.                                        |
| `lexer`                | Tokenizes source files into a token stream.                                                                                            | Zero heap allocations on hot paths; fixed-width, contiguous token slices.         |
| `parser`               | Builds a typed abstract syntax tree from the token stream.                                                                             | Arena-only for AST node construction.                                             |
| `path`                 | Canonical path value type: native-separator folding, lexical normalization, and joining.                                               | Owned strings; setup-time use only.                                               |
| `ast`                  | Abstract syntax tree node definitions shared by the parser and later stages.                                                           | Arena-allocated nodes; no independent heap allocation outside the arena.          |
| `ir`                   | Core intermediate representation: functions, blocks, instructions, operands, and types, plus storage that owns them.                   | Flat, arena-backed storage.                                                       |
| `analyzer`             | Name resolution, type checking, and ownership checking on the IR.                                                                      | No heap allocation on hot paths; operates over immutable IR views where possible. |
| `pipeline`             | Project-level build flow: package discovery, source loading, and per-file stage orchestration.                                         | Explicit phase boundaries and arena resets.                                       |
| `pkg`                  | Stands for `package`. Package manifests (`alcy.toml`), path-only dependency resolution, and lockfile model.                            | Arena-backed views; no heap allocation in the model itself.                       |
| `source`               | Source file registry: memory-mapped file loading with stable file ids.                                                                 | Mapped files plus small owned tables.                                             |
| `codegen_llvm`         | Emits LLVM IR from analyzed IR. The active MVP code-generation path.                                                                   | Local API buffers only.                                                           |
| `codegen`              | Reserved native code generation backend; no committed design yet.                                                                      | N/A — not yet implemented.                                                        |
| `core`                 | Shared configuration and utilities used across modules.                                                                                | New allocation here is an architectural decision.                                 |
| `diag`                 | Stands for `diagnostic`. Source spans, diagnostics, arena-backed bags, and the fmtlib renderer.                                        | Zero heap allocation on hot paths; message bytes use an injected arena.           |
| `base`, `debug`, `cfg` | `cfg` stands for `config`. Low-level shared facilities for numeric types, logging, diagnostics/assertion helpers, and build-time flags.| Zero heap allocations.                                                            |

Supporting targets include `tests` and `benchmarks`.

## Dependency direction

Dependency direction is part of the architecture, not merely a build-system
choice.

Higher-level modules may depend on lower-level modules, but lower-level
modules must not acquire dependencies on higher-level orchestration or policy.
New dependencies should have an architectural reason; convenience imports or
shortcuts are not sufficient justification.

In particular:

- Leaf modules such as `base`, `cfg`, and `debug` must remain independent of
  compiler pipeline stages.
- Core representations such as `ast` and `ir` must not depend on code
  generation policy.
- `codegen_llvm` depends on compiler IR and LLVM APIs, but the IR must remain
  independent of LLVM.
- Pipeline and driver code coordinate stages rather than embedding their
  implementation details into shared lower-level modules.

Avoid cyclic dependencies between modules.

## Intermediate representation

`src/ir` is the central data structure of the compiler. It models programs
as functions containing basic blocks of instructions over typed registers,
with explicit control flow (`Br`, `CondBr`, `Switch`, `Call`, `Ret`) and a
fixed set of opcodes (`src/ir/opcode.h`). Ownership-related operations
(`Move`, `Drop`) are part of the instruction set so later analyses can reason
about them uniformly.

The IR is designed around explicit data representation, stable indexing, and
invariant-preserving construction.

Key design points:

- **Interned types**: Every type reference is a `TypeIdx` into a unified type
  table (`TypeNode`: a `TypeTag` plus optional struct/array metadata).
  Primitive tags are pre-interned in tag order; composite types (`Struct`,
  `Array`) are created through builder factories.

- **Tagged operands**: An `Operand` carries its type reference together with
  its tagged payload so the tag and payload cannot drift apart. Dispatch uses
  the corresponding tag and checked accessors.

- **Structural verification**: `verify_storage()` checks structural
  invariants including index bounds, single-definition of registers,
  terminator placement, and per-opcode operand shapes. It returns
  `base::Result<void, VerifyError>`. The emitter runs verification in debug
  builds before LLVM verification.

- **Invariant-preserving construction**: Important structural invariants
  should be enforced by types, builders, and construction APIs where
  practical, rather than relying solely on comments or post-hoc validation.
  Index ranges must reference consecutive entries. Multi-entry ranges are
  built with `SeqBuilder`, which enforces contiguity; blocks requiring forward
  references use explicit backpatch setters.

The detailed IR construction contract, opcode conventions, and storage layout
are documented in [docs/ir.md](docs/ir.md).

## Memory & lifetime model

The compiler uses explicit ownership and phase-based lifetimes to keep
allocation behavior predictable.

- **Arenas & bump allocators**: AST/IR nodes, symbols, and instruction
  structures are allocated sequentially within fixed-size, chunked bump
  arenas (`core::bump_arena`) rather than by individual `new`/`delete`.
- **Identifier interning**: String literals and symbol names are interned
  during lexing into the compiler's string storage and referenced elsewhere
  through 32-bit `SymbolId` handles.
- **Per-file lifetime resets**: The pipeline clears the underlying bump arena
  at defined phase boundaries between files, eliminating individual node
  deallocation.
- **Explicit ownership**: Ownership and lifetime relationships between
  compiler data structures must remain clear across module boundaries.
  Non-owning views must not outlive the storage they reference.

The allocation model is an architectural contract on hot paths. New
allocating mechanisms in lower-level infrastructure require explicit
consideration of their effect on these contracts.

## Exception and RTTI policy

- The compiler is built with `-fno-exceptions`. Errors and expected
  recoverable failures are reported through `base::Result<T, ErrorCode>`-style
  return values or diagnostics. C++ exceptions are not part of compiler
  control flow.
- The compiler is built with `-fno-rtti`. Runtime polymorphism is not used
  in compiler code: no virtual functions or virtual inheritance. Where
  behavior varies by case, use concrete types, enum dispatch, or tagged
  unions.
- The vendored LLVM fork is itself built without RTTI and without exception
  handling; compiler code interacting with LLVM APIs must not assume either
  facility is available.

## Pipeline & data flow

```text
Source bytes
   │
   ▼
[ Lexer ]          → token buffer
   │
   ▼
[ Parser ]         → AST
   │
   ▼
[ Analyzer ]       → validated, ownership-checked IR
   │
   ▼
[ codegen_llvm ]   → LLVM IR / object code

   (codegen: native backend — reserved, not yet implemented)
```

1. **Lexing** — `lexer` reads a raw source view and produces a flat token
   buffer; tokens store fixed-width source offsets rather than line/column
   strings.

2. **Parsing** — `parser` consumes the token buffer and emits a typed AST
   into the module's arena. IR construction is a later stage.

3. **Semantic analysis** — `analyzer` operates on the IR and performs:

   * **Name resolution**: mapping interned `SymbolId`s to declarations.
   * **Type checking**: computing and verifying type signatures.
   * **Ownership analysis**: tracking `Move`/`Drop` instructions across the
     control-flow graph to enforce single-ownership guarantees.

4. **LLVM code generation** — `codegen_llvm` walks verified basic blocks and
   lowers alcy IR operations to LLVM IR.

Each stage should expose the minimum data needed by the next stage. A stage
must not reach backward into another stage's private state as a shortcut.

## LLVM integration

The compiler links against a private LLVM fork
(`third_party/llvm`, see [ADR 0002](docs/adr/0002-llvm-fork-prebuilt.md)).
The LLVM dependency is an implementation detail of the active code-generation
backend; compiler IR must remain independent of LLVM-specific types and
policies.

Only the libraries required for IR construction and emission are used. The
fork is consumed as prebuilt static libraries keyed by the submodule tag,
with a from-source fallback. Build and platform details are documented in
[docs/build.md](docs/build.md).

## System invariants

- **User-input robustness**: Invalid alcy source must result in diagnostics,
  not an assertion, undefined behavior, or process crash. Internal compiler
  invariant violations may fail fast.

- **Deterministic behavior**: Given the same source, target, compiler flags,
  and dependency versions, compilation must not depend on incidental
  nondeterminism such as iteration order, wall-clock time, or mutable
  process-global state.

- **No ambient inputs**: Builds must not depend on per-user machine state:
  environment variables, ambient locale, wall-clock time, or user identity.
  Every such input must be explicit (CLI flags, manifest, or source) so
  that identical inputs reproduce identical results on any machine.
  Diagnostic messages render in English by default; other languages are
  selected only through an explicit `--lang` flag (never ambient
  locale), keyed by stable diagnostic codes. Known tension: rendered
  source paths are currently absolute; workspace-relative rendering is
  future work, not a silent exception to this rule.

- **Configuration layering**: each build dimension lives in exactly one
  place. The manifest (`alcy.toml`, versioned and shared) declares what
  is built: targets, dependencies, and package identity. CLI flags carry
  invocation-scoped, user-specific configuration: presentation
  (`--color`, `--lang`, verbosity), local paths, and parallelism.
  Dimensions affecting outputs through a finite selection (such as the
  debug/release mode) live on the CLI as part of the build identity;
  option spaces are declared in the manifest while selection happens on
  the CLI. No dimension is configured in both places without a
  documented precedence, and environment variables are never a
  behavioral input channel.

- **Deterministic output**: Where the toolchain permits deterministic
  emission, identical inputs and build configuration should produce
  reproducible LLVM IR and object output.

- **No shared mutable pipeline state**: Compilation stages communicate
  through explicit inputs and outputs rather than mutable global state.

- **Explicit phase lifetimes**: Per-file and per-phase state must have clear
  ownership and reset points. A later stage must not rely on accidental
  lifetime extension of earlier stage state.

- **Invariant enforcement**: Structural invariants should be checked at the
  boundary where they become required, rather than relying on downstream
  consumers to recover from invalid state.

- **No untracked allocation**: New allocating utilities in low-level
  infrastructure must be evaluated against the zero-allocation contracts of
  the hot-path stages. In particular, additions to `src/core` or `src/base`
  must not silently introduce heap allocation into existing hot paths.

## Scope beyond MVP

The following do not have a committed MVP design:

- Parallel and incremental compilation.
- Custom memory management beyond the current arena model.
- Completing the `codegen` native backend or replacing LLVM/the system linker
  with a custom backend.
- Advanced optimizations and whole-program analysis.
- Language features beyond the MVP subset (defined in `docs/spec/`,
  which is normative for language behavior).

These are intentionally left open until the MVP architecture provides a
stable foundation for evaluating them.

## Bootstrap vision

The long-term project goal is to self-host the toolchain: once the MVP
compiler, build system, core/alloc/std libraries, and surrounding tools
(formatter, linter, LSP, tree-sitter grammar, setup action) are complete,
the entire toolchain is rewritten in alcy and the C++ implementation
retired.

This is outside the MVP architecture. Production code must not depend on
self-hosting assumptions, and the C++ implementation remains authoritative
until a bootstrap is deliberately introduced.
