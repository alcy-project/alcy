# ADR-0028: The Spec System

- Subject: the language
- Status: Accepted
- Date: 2026-10-01

## Context

`Map` stays `str`-keyed, `?` propagates identical errors, and ranges
do not iterate, all for one reason: the language cannot declare a
capability and let a type opt in. The plan for that has always been
`spec` — a bounded implementation mechanism behind `Hash`, `Eq`,
`Iterator`, `Display`, `Drop`, and `From` — and every slice since has
named it as the thing that arrives next. This record fixes what it
is, so the slices land against a design instead of discovering one.

The machinery underneath already exists. Inherent `impl` blocks
specialize per receiver instantiation, method calls resolve through
the receiver's nominal, `Self` substitutes in signatures and bodies,
and every monomorphization is a checked instantiation with its own
side tables. What is missing is only the capability: declaring it,
implementing it for a type, bounding a parameter by it, and
dispatching a call through it.

## Decision

**A spec is a named set of method signatures.** The declaration gives
the name, its type parameters, and `;`-terminated signatures; an
implementation gives the bodies for one type:

```alcy
spec Iterator<T> {
  fn next(self: &mut Self) -> Option<T>;
}

impl Iterator<i32> for EveryThird {
  fn next(self: &mut Self) -> Option<i32> { ... }
}
```

Receivers follow the inherent rule: exactly `Self`, `&Self`, or
`&mut Self` in first position is a method, anything else an
associated function, callable as `Type::name()`. `Self` inside the
spec and its impls satisfies the spec being defined, so a method
body calls its siblings through `self` with no further machinery.
There are no default bodies in v1: every declared method is
implemented, and an incomplete impl is an error at registration.

**Dispatch is static and scoped.** A method call resolves through
inherent impls first, exactly as today, then through spec impls.
A spec counts only when it is in scope — declared in the module,
named by a `use`, or re-exported through a prelude facade — so a
method from another package never hijacks a name silently. Inherent
lookup keeps its existing behavior; changing that is out of scope.
Lowering sees no difference between the two: a spec method is a
`MethodInfo` like any other, checked and specialized per concrete
receiver, with no vtable anywhere.

**Coherence is global.** The tree holds at most one impl of a spec
for a type, checked when the second impl registers. Two impls
overlap when their targets share a nominal whose arguments are
pairwise equal or a bare impl parameter, so `impl Display for
Point` coexists with `impl<T> Display for Vec<T>` but not with
`impl Display for Vec<i32>`. Generics never overlap each other
silently: `impl<T> S for T` overlaps everything and stands alone.
There is no orphan rule — implementations are whole-program, so a
package implements a foreign spec for a foreign type exactly when
no other impl claims the pair — and v1 targets are nominal types
only. Primitives, references, and compounds wait for a later
extension.

**Bounds constrain parameters, not definitions.** A type parameter
takes bounds where it is declared — on free functions, methods,
spec methods, and impl blocks — never on structs or enums:

```alcy
fn show_all<T: Display>(items: &[T]) -> str { ... }
```

Inside the body, `T` answers exactly the bound specs' methods, and a
call instantiates only when each argument's type implements each
bound. Multiple bounds join with `+`; there are no `where` clauses
in v1, and the `where` token stays reserved for them. Checking a
bounded body is today's generic checking with the bounds in scope,
and monomorphization is today's per-instantiation specialization
with the bound check at the call.

**`for` desugars through one rule.** Iteration needs no second spec
and no associated types: the head supplies the iterator through a
method call the existing machinery already resolves —

```alcy
for pat in head { body }
```

means

```alcy
{
  mut _it := head.into_iter()
  loop {
    match _it.next() {
      Option::Some(pat) => body,
      Option::None => break,
    }
  }
}
```

— with the static requirements that `into_iter` resolves, its
return implements `Iterator<T>`, and `T` unifies with the pattern.
`Range` exposes its iterator this way (`into_iter` returns the
cursor; the interval itself never is one), and `for`/`in` stay
reserved until the slice that implements the rule.

## Consequences

What this buys is the rest of the roadmap's vocabulary: `Iterator`
and `for`, `Hash` and `Eq` unkeying the map, `Display` for
diagnostics, `From` for `?` conversion, and eventually `Drop`
replacing today's by-name destructor recognition with a
coherence-unique impl the compiler calls as glue. None of that is
this design's scope; each is a slice with its own tests.

What it costs is a keyword and a registration table. `spec` becomes
a token matched in item position only, after the `comp` precedent
rather than the reserved list, so existing uses of the identifier
keep parsing. `for` leaves the reserved list for the `impl S for T`
position, with guidance elsewhere until its slice. The compiler
record — item and type nodes, the impl table beside the method
tables, the dispatch phases in lookup order, the catalog keys —
arrives with the first slice, as this log keeps the two apart.

What is explicitly out of scope: associated types (specs take type
parameters instead), supertraits, default bodies, spec objects
(`dyn` stays reserved), operator overloading, bounds on struct and
enum parameters, and any dynamic dispatch. Slices land in order:
declarations with implementations and dispatch; `Iterator` with
`for`; bounds with bounded-function monomorphization.
