# ADR 0001: GN as the build system

- Status: Accepted
- Date: 2026-09-16

## Context

The compiler targets Linux, macOS, and Windows with per-platform toolchains
and build modes, and needs fast incremental rebuilds during development.
CMake was already in use for building the vendored LLVM fork, but the
compiler itself needed explicit control over compiler flags, link steps,
and generated build files.

## Decision

Use GN to generate Ninja build files for the compiler itself
(`BUILD.gn`, `build/config/`, `build/toolchains/`). CMake remains in use
only for building LLVM inside the fork.

## Consequences

- Fast, hermetic-feeling builds with a single `gn gen` + `ninja` flow;
  `compile_commands.json` is generated for clangd.
- Build logic is split accordingly: GN owns the compiler, CMake owns LLVM.
  Contributors need both tools, documented in `docs/build.md`.
