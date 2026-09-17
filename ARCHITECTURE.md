# Architecture

This document describes the intended architecture of the alcy compiler at
MVP: a working compiler that translates alcy source code into executables.
Implementation details may differ while the project is under development;
this document is updated only when the design itself changes.

This document covers *what* the compiler is and *why* it's built this
way. For day-to-day build, test, lint, and code-style rules, see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Core design principles

When implementing or modifying any component in alcy, adhere strictly to the following principles:

- **Separation of Concerns & Module Boundaries**: Keep responsibilities sharply divided.
  Split files generously whenever a component takes on multiple concerns.
- **Zero Performance Overhead**: Design abstractions that compile away. Architectural elegance
  must never come at the cost of runtime performance.
- **Zero-Allocation Hot Paths**: The core compilation loop (lexing, parsing, IR transformation, analysis)
  must avoid heap allocations on hot paths (see [Memory & allocation model](#memory--allocation-model)).
- **Pragmatic Simplicity (YAGNI, DRY, KISS)**: Do not build infrastructure for
  speculative future needs. Keep implementations clear, concise, and unified.
- **Production-Ready Quality**: Do not commit prototype-quality code to the core
  pipeline. Write production-grade, fully robust C++20 from day one.
- **No Dynamic Dispatch**: `vtable` usage is strictly forbidden across the
  codebase to guarantee zero-overhead abstraction.

## Overview

alcy is a statically-typed programming language with ownership-based memory
management. The reference compiler is written in C++20 and lowers alcy
source code to LLVM IR, relying on LLVM for optimization and object code
generation.

The MVP scope is a single-threaded batch compiler: given source files, it
produces an executable. In scope: lexing, parsing, IR construction,
type/ownership checking, a linear single-pass pipeline, LLVM IR emission,
and object-file generation. Deliberately out of scope for MVP: parallel or
incremental compilation, a language server, build-system dependency
tracking, and a completed native (non-LLVM) backend — see
[Future work](#future-work).

## Repository layout

- `src/` — the compiler. One directory per module, each with a `BUILD.gn`.
- `build/` — GN build configuration: toolchains (`build/toolchains/`),
  compiler flags (`build/config/`), and helper scripts (`build/scripts/`).
- `third_party/` — vendored dependencies as submodules (`llvm`, `fpag`,
  `fmt`, `doctest`, `xxhash`), each wrapped with a `BUILD.gn`.
- `docs/` — detailed documentation, including architecture decision records
  (`docs/adr/`).

## Compiler modules

Compilation proceeds as a linear pipeline. Each stage consumes the output
of the previous one; there is no shared mutable global state between
stages beyond the data explicitly passed along.

| Module | Role | Allocation model |
|---|---|---|
| `app` | Driver: argument parsing, initialization, and pipeline orchestration. | Standard allocation (CLI parsing, file discovery only). |
| `lexer` | Tokenizes source files into a token stream. | Zero heap allocations; fixed-width, contiguous token slices. |
| `parser` | Builds a typed abstract syntax tree (AST) from the token stream. | Arena-only; emits AST nodes into the module's per-file arena. |
| `ast` | Abstract syntax tree node definitions shared by the parser and later stages. | Arena-allocated nodes; no heap allocation outside the arena. |
| `ir` | The core intermediate representation: functions, blocks, instructions, operands, and types, plus the storage that owns them. | Flat, arena-backed storage. |
| `analyzer` | Name resolution, type checking, and ownership checking on the IR. | Zero heap allocations; operates over immutable IR slices. |
| `pipeline` | Connects the stages above into a single compilation flow. | Arena reset at per-file phase boundaries. |
| `codegen_llvm` | Emits LLVM IR from analyzed IR. The active MVP code-generation path. | Local API buffers only. |
| `codegen` | Native code generation backend, reserved as an eventual alternative to LLVM. Scaffolded in the repository layout; no committed design yet (see [Future work](#future-work)). | N/A — not yet implemented. |
| `core` | Shared configuration and utilities used across modules. | Any new allocating utility here requires an ADR (see [System invariants](#system-invariants)). |
| `diag` | Source spans, diagnostics, arena-backed bags, and the fmtlib renderer. | Zero heap allocations; message bytes bump-allocated from an injected arena. |
| `base`, `debug`, `build` | Logging, diagnostics/assertion helpers, and build-time flags. | Zero heap allocations. |

Supporting targets: `tests` (unit tests per module) and `benchmarks`.

## Intermediate representation

`src/ir` is the central data structure of the compiler. It models programs
as functions containing basic blocks of instructions over typed registers,
with explicit control flow (`Br`, `CondBr`, `Switch`, `Call`, `Ret`) and a
fixed set of opcodes (`src/ir/opcode.h`). Ownership-related operations
(`Move`, `Drop`) are part of the instruction set so that later analyses
can reason about them uniformly.

Key design points:

- **Interned types**: every type reference is a `TypeIdx` into a unified
  type table (`TypeNode`: a `TypeTag` plus optional struct/array metadata).
  Primitive tags are pre-interned in tag order, so they resolve without a
  lookup; composite types (`Struct`, `Array`) are created through builder
  factories.
- **Tagged operands**: `Operand` pairs an `AutoTaggedUnion` payload with a
  type reference (12 bytes, 4-byte aligned). Tag and payload cannot drift
  apart; dispatch on `TagOf<T>` and checked `as_*` accessors.
- **Structural verification**: `verify_storage()` checks index bounds,
  single-definition of registers, terminator placement, and per-opcode
  operand shapes, returning `base::Result<void, VerifyError>`. The emitter
  runs it in debug builds before LLVM verification.
- **Construction discipline**: index ranges must reference consecutive
  entries. Multi-entry ranges are built with `SeqBuilder` (which enforces
  contiguity); blocks needing forward references use backpatch setters.
  See [docs/ir.md](docs/ir.md) for the full construction guide and opcode
  conventions.

`codegen_llvm` lowers verified IR to LLVM IR, split by concern into
orchestration/control flow (`llvm_ir_emitter.cc`), compute
(`llvm_ir_emitter_compute.cc`), and memory (`llvm_ir_emitter_memory.cc`),
with emitted values tracked in `LlvmIrStorage`.

## Memory & allocation model

Zero-allocation hot paths are enforced through three mechanisms:

- **Arenas & bump allocators**: AST/IR nodes, symbols, and instruction
  structures are allocated sequentially within fixed-size, chunked bump
  arenas (`core::bump_arena`), rather than via `malloc`/`new`.
- **Identifier interning**: string literals and symbol names are interned
  during lexing into a global, contiguously-allocated string pool,
  referenced elsewhere in the compiler as 32-bit `SymbolId` handles.
- **Per-file lifetime resets**: the pipeline clears the underlying bump
  arena at defined phase boundaries between files, eliminating individual
  node deallocation (`delete`).

## Exception and RTTI policy

- The compiler is built with `-fno-exceptions`. Errors and unrecoverable
  failures are signaled via a `base::Result<T, ErrorCode>`-style return type or
  fatal diagnostic assertions (`DCHECK` / `CHECK`) — never C++ exceptions.
- The compiler is built with `-fno-rtti`; dynamic dispatch (`vtable`s) is
  forbidden across the codebase (see [Core design principles](#core-design-principles)).
  Where behavior must vary by case, use tag-based enum dispatch or tagged
  unions instead of polymorphism.
- The vendored LLVM fork is itself built without RTTI and without
  exception handling; compiler code that interacts with LLVM APIs must not
  assume either is available.

## Pipeline & data flow

```
Source bytes
   │
   ▼
[ Lexer ]      → token buffer (contiguous, fixed-width slices)
   │
   ▼
[ Parser ]     → AST (typed syntax tree, arena-allocated)
   │
   ▼
[ Analyzer ]   → validated, ownership-checked IR
   │
   ▼
[ codegen_llvm ] → LLVM IR / object code   (active MVP path)

   (codegen: native backend — reserved, not yet implemented)
```

1. **Lexing** — `lexer` reads a raw source view and produces a flat token
   buffer; tokens store fixed-width source offsets rather than
   line/column strings.
2. **Parsing** — `parser` consumes the token buffer and emits a typed AST
   into the module's arena; IR construction happens in a later stage.
3. **Semantic analysis** — `analyzer` performs, in place, over the IR:
   - *Name resolution*: mapping interned `SymbolId`s to scope declarations.
   - *Type checking*: computing and verifying type signatures.
   - *Ownership analysis*: tracking `Move`/`Drop` instructions across the
     control-flow graph to enforce single-ownership guarantees.
4. **LLVM code generation** — `codegen_llvm` walks verified basic blocks
   and maps alcy IR opcodes directly to LLVM builder calls
   (`llvm::IRBuilder<>`).

## LLVM integration

The compiler links against a private LLVM fork
(`third_party/llvm`, see [ADR 0002](docs/adr/0002-llvm-fork-prebuilt.md)).
Only the libraries required for IR construction and emission are used;
the fork is consumed as prebuilt static libraries downloaded from GitHub
Releases, keyed by the submodule tag, with a from-source fallback.

On Windows, the prebuilt LLVM libraries and all third-party libraries link
against the static C runtime (`/MT` in Release, `/MTd` in Debug), matching
the compiler's own flags. See [docs/build.md](docs/build.md) for the setup
flow and troubleshooting.

## Build system

The build uses GN and Ninja (see [ADR 0001](docs/adr/0001-gn-build-system.md)).
Top-level targets are defined in `BUILD.gn`:

- `default` — the `alcy` compiler binary.
- `tests`, `benchmarks` — test and benchmark binaries.
- `all` — everything above.
- `//src:alcy_lib` — a complete static library (`libalcy.a` / `alcy.lib`)
  archiving every compiler module. The executables above link only against
  this target; releases ship the archive alongside the binaries (including a
  wasm build for the playground).

Platform and toolchain selection lives in `build/`; per-module build rules
live next to the sources. See [CONTRIBUTING.md](CONTRIBUTING.md) for the
day-to-day build, test, and lint commands, and for the CI matrix.

## System invariants

- **Crash & diagnostic discipline**: compiling invalid user source code
  must never crash the compiler process; diagnostics are gathered,
  formatted via `fmt`, and reported gracefully.
- **Deterministic builds**: given the same input source, target triple,
  and compiler flags, alcy must produce bit-for-bit reproducible LLVM IR
  and object output.
- **Zero untracked allocation**: any new allocating utility added to
  `src/core` or `src/base` requires an explicit ADR, since every
  hot-path stage depends on these zero-dependency leaf modules.

Code style, tooling, and CI enforcement are contributor-workflow concerns
and live in [CONTRIBUTING.md](CONTRIBUTING.md), not here.

## Future work

The following are deliberately out of MVP scope and have no committed
design yet:

- Parallel and incremental compilation.
- Custom memory management beyond the current arena model (e.g.
  virtual-memory-backed storage).
- Completing the `codegen` native backend, or a custom linker, as an
  alternative to LLVM and the system linker.
- Advanced optimizations and whole-program analysis.
- Language features beyond the MVP subset (to be defined alongside a
  language specification).
