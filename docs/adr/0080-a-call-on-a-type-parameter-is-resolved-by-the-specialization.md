# ADR-0080: A call on a type parameter is resolved by the specialization

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-21

## Context

ADR-0079 decided that a generic body is checked once, against its
parameters, and that the dependent instantiates the checked body. That
leaves one question open, and it is the one the record named: how a call
the parameter's type decides is resolved.

The language already has the shape of the answer. A type parameter takes
bounds where it is declared, and inside the body `T` answers the bound
specs' methods (`docs/spec/types.md`, ADR-0028). So a body that calls
`x.show()` on a `T: Display` parameter is calling a method the bound
declares, and the call site of the generic checks that the argument's
type implements the bound.

What does not exist yet is the call itself. Today a generic body is
checked per instantiation, so `x.show()` resolves against the
concrete type and the call is an ordinary call to an ordinary
function. Under a body checked once, the parameter has no concrete
type, so the call has no callee: the body knows only that the bound's
spec declares the method, and the concrete implementation is a fact
about the argument, which the body does not have.

The alternatives are the ones a monomorphizing language is measured
against. A dictionary of implementations beside the value (Rust's
`dyn`, Go's interface values) is a runtime cost: an indirect call, a
table per instantiation, and a type's implementations that cannot be
known when the code is written. Erasure to a single implementation
makes a generic body mean one thing, which is not what a generic
means.

## Decision

**A call on a type parameter is a dispatch in the parametric form, and
the specialization resolves it to a direct call.**

Three parts.

**The parametric IR records a dispatch.** A call whose receiver's type is
a type parameter names the spec and the method, rather than a function:
the callee is "the method of that spec, on the value of that
parameter". The parametric form carries this alongside the type
parameter itself, which is a type node naming the parameter's index.

**The checker resolves a dispatch against the bound.** The parameter's
bounds declare which specs its type answers, and a dispatch is
well-formed when the bound's spec declares the method. A body that
calls a method its bound does not have is refused at the definition,
where the bound is written, rather than at a caller's line.

**The specialization resolves a dispatch to the concrete
implementation.** The pass knows the argument's type, finds the
implementation of the spec for that type, and rewrites the call to the
implementation's function, which the pass then instantiates in turn.
The emitted code therefore holds no dispatch: every call in it is a
direct call to a monomorphic body, inlinable and devirtualized by
construction, with no table and no dictionary.

**A bound's satisfaction is checked where the generic is
instantiated.** The rule ADR-0028 already has: the call checks that
the argument's type implements the bound, and names the caller's type
when it does not. The specialization can therefore assume the
implementation exists, because no instantiation that reaches it was
admitted without one.

## Consequences

A generic body is checked once and means what it says: its calls are
the calls its bounds allow, and the concrete implementation is a
property of the argument rather than of the body.

The emitted code pays nothing for the abstraction. A dispatch exists
only in the parametric form, which no backend ever sees, and it is
replaced by a direct call before the code is generated.

The cost is in the compiler. The parametric IR grows a form the
verifier must accept and the backend must never meet, the
specialization pass must resolve a dispatch as well as substitute
types, and a dispatch on a parameter whose implementation is itself
generic sends the pass back through the instantiation graph.

What this record does not settle: whether a dispatch may appear on a
value whose type is a parameter *bound to* another parameter (a `T`
whose bound mentions `U`), which the pass would have to resolve in an
order the instantiation graph does not yet have; and whether a spec may
be sealed to a package in a way that keeps a dispatch from being
resolved (ADR-0053), which is a question about sealing rather than
about dispatch.

## Staged landing

The checker's half landed with the definition-site check (ADR-0079
slice two): a call on a parameter resolves through the parameter's
declared bounds - a bound that refines the required spec provides its
methods, and a bound a call needs but the parameter does not declare
is refused at the definition. The filled signature is the row the call
checks against, carrying the parameter as its self type. What remains
is the IR's half: the parametric form records the dispatch rather than
a resolved row, and the specialization resolves it to a concrete
implementation.
