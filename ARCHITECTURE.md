# Architecture

This document describes the intended architecture of the alcy compiler at
MVP: a working compiler that translates alcy source code into executables.
Implementation details may differ while the project is under development;
this document is updated only when the design itself changes.

## Overview

alcy is a statically-typed programming language with ownership-based memory
management. The reference compiler is written in C++20 and lowers alcy
source code to LLVM IR, relying on LLVM for optimization and object code
generation.

The MVP scope is a single-threaded batch compiler: given source files, it
produces an executable. Performance work, parallelism, and language
extensions are explicitly out of scope (see Future work).

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

| Module | Role |
|---|---|
| `app` | Driver: argument parsing, initialization, and pipeline orchestration. |
| `lexer` | Tokenizes source files into a token stream. |
| `parser` | Builds a high-level representation from the token stream. |
| `ir` | The core intermediate representation: functions, blocks, instructions, operands, and types, plus the storage that owns them. |
| `analyzer` | Name resolution, type checking, and ownership checking on the IR. |
| `pipeline` | Connects the stages above into a single compilation flow. |
| `codegen_llvm` | Emits LLVM IR from analyzed IR. |
| `codegen` | Native code generation backend (alternative to LLVM). |
| `core` | Shared configuration and utilities used across modules. |
| `base`, `debug`, `build` | Logging, diagnostics/assertion helpers, and build-time flags. |

Supporting targets: `tests` (unit tests per module) and `benchmarks`.

## Intermediate representation

`src/ir` is the central data structure of the compiler. It models programs
as functions containing basic blocks of instructions over typed registers,
with explicit control flow (`Br`, `CondBr`, `Switch`, `Call`, `Ret`) and a
fixed set of opcodes (`src/ir/opcode.h`). Ownership-related operations
(`Move`, `Drop`) are part of the instruction set so that later analyses
can reason about them uniformly.

## LLVM integration

The compiler links against a private LLVM fork
(`third_party/llvm`, see [ADR 0002](docs/adr/0002-llvm-fork-prebuilt.md)).
Only the libraries required for IR construction and emission are used;
the fork is consumed as prebuilt static libraries downloaded from GitHub
Releases, keyed by the submodule tag, with a from-source fallback.

On Windows the prebuilt LLVM libraries use the static C runtime (`/MT`),
matching the compiler's own flags. See [docs/build.md](docs/build.md) for
the setup flow and troubleshooting.

## Build system

The build uses GN and Ninja (see [ADR 0001](docs/adr/0001-gn-build-system.md)).
Top-level targets are defined in `BUILD.gn`:

- `default` — the `alcy` compiler binary.
- `tests`, `benchmarks` — test and benchmark binaries.
- `all` — everything above.

Platform and toolchain selection lives in `build/`; per-module build rules
live next to the sources. CI builds debug and release configurations on
Linux, macOS, and Windows.

## Conventions

- C++20, no exceptions (`-fno-exceptions`).
- LLVM libraries are built without RTTI and without EH; compiler code that
  interacts with LLVM APIs must not rely on either.
- Formatting and static analysis are enforced by CI: clang-format,
  clang-tidy, cpplint, and typos. Run `./build/scripts/lint.py` and
  `./build/scripts/format.py` before submitting changes.

## Future work

The following are deliberately out of MVP scope and have no committed
design yet:

- Parallel and incremental compilation.
- Custom memory management (arena allocators, virtual-memory-backed storage).
- A native backend or custom linker replacing LLVM and the system linker.
- Advanced optimizations and whole-program analysis.
- Language features beyond the MVP subset (to be defined alongside a
  language specification).
