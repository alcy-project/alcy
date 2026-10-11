# ADR-0063: The release passes live in the pipeline

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-12

## Context

Optimization was LLVM's: `--release` ran the O3 middle end inside
`codegen_llvm`, and a backend without a middle end (the direct wasm
emitter, ADR-0049) refused a release build rather than emit
unoptimized code under a name that promises otherwise. That left two
problems. Optimizations the frontend's own information enables —
ownership facts, monomorphization, the language's own constants —
either had to be re-derived by LLVM or could not happen at all, and
the direct backend could never be released. The IR also keeps facts
LLVM never sees once they are lowered away, so the passes that use
them belong above the backends.

## Decision

**The pipeline owns an IR pass stage between borrow checking and
codegen, and `--release` is what turns it on.**

- `compiler/opt` holds the passes and runs them over the verified
  lowered storage; the result is rebuilt through the verifier, so a
  backend still receives proved IR. A pass that fails verification is
  a compiler bug, reported as such.
- The stage runs after borrow checking. The passes rewrite values
  rather than ownership, so the proof borrow produced still holds, and
  both backends — including the direct one — see the same optimized
  IR. A future pass that needs ownership facts consumes what borrow
  already computed.
- The first passes are function-local and deliberately conservative:
  integer constant folding with propagation into every use (registers
  are single-definition, so a substitution is valid for the whole
  function), then pruning of the instructions folding left without a
  use. Division by zero is not folded, because the language's panic is
  the result; `Load` and the ownership markers are never pruned.
- The passes must not depend on the build profile or on the named
  machine beyond the pointer width. The cache boundary stays below
  this stage: a future IR cache stores the unoptimized lowered
  artifact, and the passes run on every load.
- LLVM's middle end keeps running for its own backend; the passes
  here exist for what LLVM cannot see and for the backend that has no
  middle end at all.

## Consequences

`--release` with a direct backend now emits an optimized module
instead of an error (ADR-0049, amended). For the LLVM backend the
passes overlap O3 and are release-gated, so debug builds pay nothing;
a release build pays one walk over each function. The first passes
are small on purpose: larger ones — devirtualization, bounds-check
elision, allocation pairing, drop elaboration — need cross-block or
cross-function facts, and each lands as its own pass with its own
tests. The unoptimized IR forms (`--emit=ir`, `--emit=ir-bc`) still
refuse `--release`, because the form's contract is the cacheable,
unoptimized artifact.
