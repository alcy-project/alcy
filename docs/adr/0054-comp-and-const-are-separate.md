# ADR-0054: comp and const are separate

- Subject: the language
- Status: Accepted
- Date: 2026-10-07

## Context

`comp.md` landed the bootstrap stage: `comp` annotates parameters,
declarations, and blocks, evaluation is bounded and AST-based, and
`comp fn`, const-position extensions, and value parameters are
deferred. `const` items are literal-only. Those deferrals now block
the roadmap: `ArrayVec<T, N>` needs a value in a type, comp functions
need a marker, and the evaluator needs a home outside the lowerer,
which the backlog's "Split Lowerer" entry already names.

Two words are in play and they answer different questions. `const`
answers what a value is; `comp` answers when code runs. The bootstrap
stage blurred them: `comp x := ...` declares a value, while
`comp { ... }` evaluates a block.

## Decision

**`const` declares a compile-time value.** Item and local position
alike: `const X: T = expr` and `const x := expr`. The initializer is
evaluated at compile time, must terminate within the evaluation
bound, and is pure: no runtime storage, no statics, no I/O. A `const`
value is usable wherever the language needs a value during
compilation - an array length, a value parameter, a `comp` argument -
and as an ordinary value in code. It is immutable.

**`comp` only directs evaluation.** Three sites remain: a `comp`
parameter specializes a call per comp-known argument; `comp { ... }`
evaluates its body at compile time and splices the value; and
`comp fn` is below. `comp x := ...` retires, and compile-time mutable
state is an ordinary `mut` local inside comp evaluation, which cannot
escape it.

**`comp fn` calls are compile-time only.** A `comp fn`'s arguments
must be comp-known, the call itself is an error outside comp
evaluation, and the result splices as a comp-known value. It is a
free function first; a receiver method keeps the marker for a later
slice. There is no `const fn`, so `comp` is the single word for code
evaluated during compilation.

**Values in type positions are `const`.** `[T; N]` takes a `const`
value and a declaration may take value parameters (`struct
ArrayVec<T, N>`, `fn fill<T, N>(...)`). `comp` never appears in a
type. `ArrayVec` is the acceptance case for the slice.

**One evaluator, two readers, two engines.** The evaluator becomes
its own component. The checker calls it for the `const` values that
types need; lowering calls it for comp blocks, specialization, and
`fmt` expansion. The first engine is today's AST evaluation, lifted
out of the lowerer. The second is an IR interpreter: the requested
function or block is lowered through the ordinary pipeline and then
interpreted as verified IR, behind the same interface, so comp code
has the language's semantics - borrowing, drops, integer rules - and
can call what ordinary code calls. The two engines are compared over
the corpus before the switch, and the AST engine retires. An engine
is deterministic, bounded by steps and recursion depth, memoized by
instantiation and comp arguments, and allocates only to materialize
constant data; I/O, `panic`, and reads of `static` are compile-time
errors, as they are today.

## Consequences

`comp.md` is rewritten at implementation; the grammar gains local
`const` declarations and keeps `comp` where it is. `const` items lose
their literal restriction, and comp-known values gain a name that says
what they are.

The IR interpreter is the largest slice, and it is what makes the
semantics one thing instead of two. Until it lands, the AST engine
carries the contract and the corpus keeps the two comparable.
`ArrayVec`, the old roadmap item, lands as this item's acceptance case
once value parameters work.
