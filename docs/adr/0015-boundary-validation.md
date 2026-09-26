# ADR-0015: Validation happens at public API boundaries

- Status: Accepted
- Date: 2026-09-26

## Context

Validation was spread unevenly across the compiler. Some entries validated
their input, most did not, and several public APIs signaled failure with
sentinels that callers could not distinguish from legitimate values:

- `std_prelude` returned an empty `std::span` both for "no files found" and
  for "loading failed"; `stage_runtime` returned `bool`.
- `SourceManager::bytes`/`name` answered an invalid `FileId` with an empty
  view, indistinguishable from a registered empty file.
- `DiagBag::at`/`label`, the `StorageBuilder` setters, and parser token
  access relied on `DCHECK`, which is compiled out in release builds.
- Test code constructs AST, manifest, module, and IR data directly, so
  malformed data reaches consumers without passing any entry point.
- `diag::Fallible<T>` hid *which* error type a fallible API used behind a
  container alias, and `RunResult`/`BuildResult` mixed infrastructure
  failure with program exit codes.

The project invariant already required user-input robustness ("invalid alcy
source must result in diagnostics, not an assertion"), but nothing stated
where validation belongs or what a public signature promises.

## Decision

**Validation is performed at trust boundaries - the public API of each
module - and nowhere else by default.**

Concretely:

1. **Public API is checked-only.** Every public entry point that accepts
   externally supplied or independently constructible data returns
   `base::Result<T, E>` (from `fpag/base/result.h`). There is no public
   unchecked API; internal unchecked implementations stay in private
   headers (`*_internal.h`) or `.cc` files.

2. **One rule chooses `E`:**
   - Callers that must handle the failure programmatically get a
     module-local error type: `path::PathError`, `source::SourceError`,
     `pipeline::SpawnError`, `ir::VerifyError`, ...
   - APIs that accumulate diagnostics in a `diag::DiagBag` and only need to
     convey success/failure return `base::Result<T, diag::Reported>`.
     `Reported` is a zero-sized marker meaning "the details are already in
     the bag".
   - `diag::Fatal` and the `diag::Fallible<T>` alias are removed. The
     container alias is never reintroduced: the error type stays visible in
     every signature.

3. **Verifiers are pure.** `verify_*` functions take their input by
   `const` reference, return `base::Result<void, VerifyError>` (or a
   validated artifact), and perform no input mutation, I/O, logging,
   `DiagBag` writes, or global state access. The first structural error is
   returned; the caller decides how to turn it into a diagnostic.

4. **Public entries call the verifier and convert failure.** The entry
   emits a diagnostic for the structured error and returns early; the
   private implementation never sees invalid input.

5. **Verified artifacts travel through hot paths.** When several consumers
   need the same guarantee, the validating entry returns a module-local
   proof-carrying type. Introduced: `ir::VerifiedStorage` (checker and
   lowering outputs; borrow checking and the emitter consume the proof
   instead of re-running the verifier). Single-consumer checks stay as
   plain `verify_*` at their boundary (`ast::verify_file`,
   `lexer::verify_token_stream`, `analyzer::verify_module_tree`,
   `pkg::verify_manifest`): a proof type pays off only when the same
   guarantee crosses several consumers. Local index/range invariants
   remain `DCHECK`s; they are not re-checked across stage boundaries.

6. **Responsibilities are fixed.** The `cli` parses argv grammar, validates
   flag values, builds `CliConfig`, dispatches, and maps exit codes - it
   performs no semantic validation of source, manifests, module graphs, or
   IR. Pipeline entries (`build_*`, `check_*`, `run_*`, `std_prelude`,
   `stage_runtime`, `find_package_manifest`) validate their raw request once
   and pass validated targets down; CLI and pipeline never duplicate the
   same path or manifest check.

## Consequences

- Malformed data constructed directly by test code is rejected by the same
  public API that rejects malformed user input, so the invariant holds
  without test-only entry points.
- Every failure path is visible in the signature: no sentinel overloads of
  meaning, no alias hiding the error type, no release-build-only crashes
  on boundary input.
- Hot paths do not pay for validation twice: full verification runs once
  at the boundary and the proof-carrying type carries it forward.
- Cost: ~every public signature gains a `Result`, which touches call sites
  across the tree; and each validated artifact is a new type to construct.
  Pre-MVP, the breaking changes are accepted in one sweep rather than
  through compatibility shims.
- Out of scope: `ValidationMode`/`UncheckedContext` style global modes,
  filesystem existence checks in `path`, trivial accessor wrapping
  (`operator[]`, enum getters), and benchmarks.
