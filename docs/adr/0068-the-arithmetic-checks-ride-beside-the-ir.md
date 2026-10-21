# ADR-0068: The arithmetic checks ride beside the IR

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-13

## Context

ADR-0065 made the overflow policy a property of the build, and the
first landing read it where the checks were emitted: `codegen_llvm`
consulted `EmitRequest::overflow_checks` and LLVM built every checked
operation, while the direct wasm backend could not and a debug wasm
build wrapped (the gap is a backlog entry). The policy had leaked
into a backend, so a second backend could agree with the first only
by re-deriving the same rules from the same boolean.

One condition cannot be re-derived from the lowered IR at all.
Lowering masks a shift count to the width (ADR-0066) before the
shift, so an emitter that sees the shift no longer holds the count
the source wrote, and "an overshift aborts" has nothing left to test.
`MIN / -1` is the same shape: lowering replaces the divisor before
the division.

## Decision

**One walk over the shared IR turns the build's policy into an
overlay beside it, and each backend realizes the checks from the
table.**

- `src/overlay` computes the overlay once per build, over the
  whole IR and after the release passes (ADR-0067) have run. The IR
  is neither copied nor rewritten: the definitions of ADR-0066 stay
  in it, and the overlay says only which operations the build checks
  and which registers hold the values a check reads.
- An entry names the instruction and the check's kind: an overflow in
  `+`, `-`, and `*`, a shift count at or past the width, or a signed
  `MIN / -1`. A kind that is not the instruction's own operands
  carries the register its condition reads — a masked shift's raw
  count, a signed division's raw divisor — which is the walk's whole
  reason to exist. The panic site is the instruction's own span,
  rendered the way call sites already are.
- A backend never reads `overflow-checks`; it reads entries. The LLVM
  emitter realizes an overflow entry with the `with.overflow`
  intrinsic and the check helper, and the direct wasm backend
  realizes the same entries with its own comparisons and its runtime.
  The two realizations repeat only the condition each kind names, and
  the execution cases run on both backends so a disagreement is a
  test failure, not a discovery.
- `overflow-checks` is a profile key beside `optimize` and `on-panic`:
  `debug` and the baseline say on, `release` says off, and
  `--overflow-checks=on|off` overrides it the way `--on-panic` does.
  `--optimize=on|off` arrives with it, so every profile key has a
  flag.
- The unoptimized IR forms stop refusing `--release` (ADR-0067,
  amended): the form is the cacheable, profile-independent IR, and
  the overlay is exactly what may differ after it.
- Compile-time evaluation is untouched: it always runs checked
  (ADR-0065), and its diagnostics never consult the overlay.

## Consequences

- Debug and release share one verified IR and one walk; the overlay
  is one small entry per affected instruction and dies with the
  build, like the passes' rebuilt storage (ADR-0067).
- An overshift both masks and names itself: the value follows
  ADR-0066 in every build, and a checked build aborts on the count
  the source wrote before the masked shift runs.
- A checked `MIN / -1` aborts with "integer overflow" instead of
  wrapping; an unchecked one keeps the wrap select. `MIN % -1` stays
  zero in every build - its value is defined and no check is owed.
- The direct wasm backend closes the gap ADR-0065 recorded; the
  backlog entry goes.
- The playground compiles with the checks on, the way `alcy check`
  and a default build do, so a snippet's overflow reports itself.

## Alternatives considered

- **A backend reads the policy boolean** (the first landing).
  Rejected: two backends deriving the conditions from one flag is a
  place to disagree, and the overshift condition is not derivable
  after lowering has masked the count.
- **The overlay carries replacement IR** - a rewritten storage, or
  fragments the emitter splices. Rejected: the definitions are
  already in the IR, and a check is a straight-line call the emitters
  already know how to make, so a fragment interpreter would be
  machinery without a payer.
- **A generated check function per kind and type, called from the
  emitter.** Rejected: a call per checked operation taxes debug
  builds, and the side storage would make every emission carry more
  than one storage.
- **An instruction flag in the IR** (ADR-0065). Rejected there and
  still: profile content in the IR keys a second cache entry.
