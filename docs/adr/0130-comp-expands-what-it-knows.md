# ADR-0130: `comp` expands what it knows

- Subject: the language
- Status: Accepted
- Date: 2026-10-30

## Context

`fmt` is a compiler feature with an alcy facade. `analyzer/fmt.h`'s
`parse_format_string` decodes the format string at compile time and is
shared by the checker and the lowering; `Fmt::check_arguments` checks
each placeholder against the argument's type; and `Fmt::emit_pieces`
(about twelve hundred lines) emits literal copies, integer digit
loops, and the float decimal-and-mantissa conversion as IR, per call
site. Only a nominal argument's conversion runs alcy code (its
`Display` implementation). `io::print` and its three siblings expand
into runtime hooks (`alcy_print` and friends). The declared bodies in
`lib/std/fmt` are `panic` guards: they never execute, because the
language cannot express them.

`comp` (ADR-0054, ADR-0102) evaluates comp-known values and splices a
value. `fmt` needs two things it does not have: a branch on an
argument's *type*, and the *emission* of runtime code (copies, loops,
branches, buffer arithmetic). Neither is a value computation. Rust
does not solve this either - `format_args!` is compiler machinery
plus a macro system - and C++'s `fmt` library solves it with macros
and templates. The language can do better because it owns both
halves: compile-time evaluation and the checker.

`fmt` is not the only client. The same missing mechanism carries the
struct flavor of input (a read per field), a clap-like argument
parser (a read per field, with the field's name), and serde-like
(de)serialization (arbitrary nesting). One mechanism, not four
compiler special cases. The evaluator is closer than it looks: `comp`
already walks strings (`str_len`, `str_byte`, `str_slice`), reads
types-only facts (`size_of`, `align_of`), builds struct and array
values, and runs bounded and deterministic under a quota.

## Decision

**`comp` expands: comp-known values drive compile-time control flow,
and the operations that cannot be evaluated are emitted as runtime
code. It is partial evaluation, written in the language and checked
by the ordinary rules.**

- Compile-time control flow is explicit: `comp for` and `comp if`
  mark the branches and unrolls that the compiler evaluates. The
  existing rule that control flow must not cross a `comp` boundary
  extends to emission.
- The bounds of ADR-0102 are unchanged: no I/O, no clock, no
  environment, no static storage, no foreign call; bounded and
  deterministic evaluation; checked arithmetic; a quota. An
  expansion-size cap joins the quota, because unrolling is now a way
  to spend code.
- It is not a macro system (ADR-0097's floor stays): arguments are
  values, there is no token substitution, nothing runs in the
  compiler outside the checked language, and the emitted code is
  checked per instantiation like any generic body (ADR-0079,
  ADR-0080).
- Placement: the emitted code is the function's own
  per-instantiation body, not statements spliced into the caller's
  scope. A `comp` parameter already specializes a function per call
  ("monomorphization-like, as-if"), so the reading of as-if becomes
  "the specialized body". Inlining is the toolchain's decision; an
  inline marker is added only if the pilot measures a need.
- Hygiene and diagnostics: temporaries are compiler-generated; a
  failure inside evaluation diagnoses at the comp code's span; a
  dispatch failure inside emitted code diagnoses at the call's
  argument, where the type that failed is written.
- The language's word for this model is *expansion*. "Staged" stays
  the codebase's word for the toolchain-provided standard library
  (the `is_staged` flag); renaming that flag is optional cleanup, not
  part of this decision.
- **`fmt` is the pilot.** The format string parses in comp alcy; the
  conversions become spec-dispatched alcy (the builtins gain their
  implementations; a float carries a precision, so its conversion
  takes the precision as an argument - the exact contract is fixed
  in the pilot); the C++ conversion lane retires once parity holds.
  `io::print` and its siblings ride the same expansion while the
  hooks keep the write-all loop (ADR-0023). The `panic` guards
  become real bodies.
- Parity is a gate, not a hope: the existing fmt tests are the
  oracle, joined by code size, compile time, runtime performance,
  wasm parity, and `backlog.md`'s float-digit accuracy item.
- The pilot fixes two preconditions on the language: compile-time
  access to the variadic tuple's shape (its length, and indexing by
  a comp-known index that keeps each element's type), and a
  restatement of ADR-0055's rule that only compiler-expanded
  functions may declare `..`. Once expansion is general, `fmt` and
  `io` are ordinary instances of it, and a user's staged function
  may declare one.
- Reflection - the struct-shaped clients' other half - is its own
  record when it is designed: read-only, comp-only, no runtime type
  information, no generative types. Input, clap-like parsing, and
  serde-like code need it; `fmt` does not.

## Consequences

- `fmt`'s policy and algorithms have one home, `lib/std/fmt`, and
  there is no second implementation in C++ to drift from it. The
  other three clients become libraries rather than compiler lanes.
- Compile-time cost, code size, and the generated code's shape
  become things the language can spend; the quota and the cap make
  the spending bounded and deterministic.
- `docs/spec/comp.md`'s "as-if semantics; no code-shape contract is
  promised" is restated: the language still promises no particular
  shape, but expansion is deterministic, bounded, and observable in
  diagnostics rather than hidden.
- The special lane that recognized `fmt`'s and `io`'s items by
  package generalizes; what remains package-recognized is at most a
  manifest declaration of which items expand.

## Alternatives considered

- **Keep the compiler lane.** Rejected: `fmt` stays C++, and the
  other three clients cannot be built on it at all.
- **A plan computed in comp and interpreted in C++.** Not taken as
  the end state: the plan is a second IR whose interpreter grows a
  C++ case per feature, which is the current problem with extra
  steps. It is allowed as a temporary shape for `fmt` alone if the
  pilot wants one.
- **Call-site splicing (the `fn`'s comp context inlined into the
  caller).** Not taken: hygiene, diagnostics, and code shape all
  become the caller's problem, and the function stops being a
  function. Revisit only if measurement shows inlining must be
  guaranteed.
- **Macros (by example or procedural).** Rejected in ADR-0097; the
  floor there stays closed.
- **Zig's full comptime, with types as first-class values and
  generative types.** Not taken wholesale: none of the four clients
  needs to construct a type, and the smaller step - expansion now,
  a read-only reflection form later - keeps the type system's
  guarantees intact.

## Deferred

- Reflection's record and its floor.
- `[T; n]` in declarations: attributes (deferred for `repr(C)`) or
  wrapper types (ADR-0129) until then.
- The `Parse` capability's associated functions (recorded with the
  implementation, ADR-0132).
- Spec default methods, which are what the iterator adapters would
  need to become methods (ADR-0132).
- The evaluator's own deferred list (receiver methods marked
  `comp`, float evaluation, generics interplay) stays; the pilot
  needs none of it.
