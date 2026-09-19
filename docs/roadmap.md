# MVP Roadmap

Milestone-gated stage plan from the current C++ toolchain to a
self-contained MVP compiler. No dates: research items (notably the
borrow checker) cannot be estimated honestly. Phase details are
briefed just in time; pivots go through ADRs.

MVP completion means: the demo set builds and runs on all three OSes
via `alcy build`, `alcy check` covers the frontend, CI is green, and
`docs/spec/` matches the implementation with zero drift.

## Phase A — Frontend

Lexer (`keywords.md`), parser (`grammar.md`), shadowing desugar,
module tree and name resolution (`mod`/`use`/visibility). Decide the
`print` intrinsic surface here.

- Exit: `alcy check` resolves modules on demo inputs.

## Phase B — Core type system

Numerics, structs, enums, tuples, blessed `Result`/`Option`
monomorphization, structural `Copy`, overflow semantics, `match`
exhaustiveness.

- Exit: typed AST/IR type checking over the demo set.

## Phase C — Lowering and borrow checking (thesis risk)

Ownership is checked on the IR (no AST-based borrow checking: that
would be throwaway once lowering lands). This phase therefore covers
the first half of lowering, sliced straight-line first so the thesis
skeleton proves out early, then borrow checking on verifier-green IR:
borrow expressions, the lowering slice (straight-line code, calls,
aggregates, references, `Move` insertion), intraprocedural NLL
(move checking, loan invalidation, exclusivity over CFG with dense
bitsets), control-flow lowering, and summary computation with
call-site instantiation.

- Exit: borrow diagnostics plus summary propagation tests, all on
  verifier-green IR.

## Phase D — Codegen and runtime floor

Remaining `codegen_llvm` opcode coverage, layouts, strings, the
panic abort path, `main` forms, and the `print` intrinsic (lowered
to a write syscall; migrates to a core function once FFI lands).
Any remaining AST-to-IR lowering lands here too.

- Exit: LLVM IR to objects to executables, end to end.

## Phase E — Driver integration and core seed

End-to-end `alcy build` for single-package programs, diagnostic
quality, exit codes. Core library ships minimal (prelude reserved).

- Exit: the demo set builds and runs.

## Phase F — Polish

Warning families (unreachable, `must_use`, unreachable files),
example harness, documentation, and the finalized acceptance demos.

## Explicitly not committed

- A separate HIR: revisit only if match-lowering complexity,
  optimization passes, or region precision demand it.
- Timelines for any phase above.
- Post-MVP tracks from `docs/spec/deferred.md`, which stay out
  regardless of MVP progress.
