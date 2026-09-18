# ADR-0005: LLVM-Only MVP Backend

- Status: Accepted
- Date: 2026-09-18

## Context

A custom native backend (fast debug builds, flash linking, custom
linker) is part of the long-term performance hypothesis, but it is a
research-scale project that would block MVP behind backend work while
the language itself is still being defined.

## Decision

- MVP code generation targets LLVM IR only, in both debug (`-O0`
  equivalent) and release configurations.
- The custom backend, in-memory linker, and incremental linking are
  post-MVP tracks. No MVP architecture decision may assume them.

## Consequences

- Debug-build speed is bounded by LLVM `-O0` for MVP; this is accepted
  explicitly, not discovered later.
- The `codegen` module remains reserved scaffolding with no committed
  design.
