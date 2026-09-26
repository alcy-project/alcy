# ADR-0012: Reborrow on Reference Read, and How the Extent Is Computed

- Status: Accepted
- Date: 2026-09-26

## Context

Reading a place of reference type currently produces a bare pointer that
nothing constrains. A `Borrow` instruction creates a loan on a place, but
a `Load` from a place whose type is a reference does not. So a `&mut T`
sitting in a local, a field, or a parameter can be copied out and used to
mutate through, with no loan anywhere and no conflict to report.

That is not hypothetical. A growable container with a read accessor
written as `fn at(self: &Self, index: usize) -> Option<T>` read the
`&mut MaybeUninit<T>` field out of a shared borrow of the owner and
produced a `&mut T` from it. The borrow checker accepted it. The program
returned garbage.

Two more facts about the current pipeline shape the fix.

A reference read is already a **move**. The lowerer emits a `Move` when
it reads a reference-typed value, so `&mut T` is already move-only in
practice: reading one consumes it, and using it twice requires a
reborrow. Move-only is not a change to make; it is a property the
reborrow design has to fit.

Loans already carry a `birth`/`expiry` extent and the checker already
computes `last_use`, and a function already carries a summary of which
parameters' loans may reach a return. So the extent question and the
"escaping borrow" question are not starting from nothing.

## Decision

### The rules

These are settled; the remaining work is implementing them.

1. **Reading a place of reference type reborrows the referent.** The
   read is not a copy of a pointer; it is a loan on the place the
   reference points into, valid exactly as long as the enclosing borrow
   of that place.
2. **Reborrowing is implicit** at function-argument and method-receiver
   positions.
3. **`&mut T` coerces to `&T`.** A shared reborrow of a unique borrow.
4. **`&mut T` is move-only.** Forced: while it is `Copy`, `b = a` yields
   two live unique borrows of one place, and no sound model keeps it
   `Copy`. It is already move-only in the pipeline, so this documents
   rather than changes.
5. **A shared borrow freezes the whole referent, not one field.** A
   `&Vec<T>` prevents a `push` through any path, not just through the
   reference. Field-sensitive sharing is where aliasing defects come
   from, and the language has no interior mutability to make it safe.
6. **A returned reference borrows from the receiver** by elision, the
   rule Rust uses. Without it `fn at(self: &Self, i: usize) ->
   Option<&T>` is inexpressible and no read-only accessor exists.
7. **`dealloc` keeps taking its block by value**, consuming it, which is
   rule 4 applied.

### The extent is non-lexical

A loan ends at its last use, not at the end of its block. This is
decided, and it is decided because the language's stated goal is region
inference: a borrow checker that rejects correct programs works against
that goal, and the self-hosting compiler would spend its first weeks
arguing with it. A loan's extent is the input to a region solver rather
than a syntactic approximation of one, so the two share a computation
rather than merely agreeing.

### Two-phase borrows are deferred

`v.push(v.len())` needs the receiver's mutable borrow reserved during
argument evaluation and activated after. It is an ergonomics patch on a
correct checker, not a soundness prerequisite, so it follows the
reborrow work rather than blocking it. The compiler will hit it early;
it should not wait long.

## Where the implementation actually stands

The rules above are not implemented. What is established about the
current code, and what the work has to account for:

- A `Load` from a reference-typed place creates no loan. This is the
  hole.
- The obvious fix — treat such a `Load` like a `Borrow` for loan
  purposes — was tried and **rejected**, for two reasons worth recording
  so the next attempt does not repeat them.

  First, it reports false conflicts on correct code. A reborrow derived
  from `m` is the *same* borrow as `m`'s own loan, not a competing one;
  the derived loans are already tracked in the instruction's flow set and
  have to be excluded from the conflict test. Checking that exclusion
  alone was not sufficient, because the reference read is also a move and
  the move machinery already records the place as consumed.

  Second, and decisively, it **does not catch the case it was written
  for**. A reborrow that escapes through a return flows out through the
  summary mechanism — the set of parameters whose loans may reach a
  return — and a per-instruction conflict test never sees it. The
  original unsound program still compiled.

So the fix is not a per-instruction check. It has to be part of the
reborrow machinery itself: a reborrow has to shorten the loan it
derives from for the extent of the reborrow, a loan must be able to
carry a *region* rather than a fixed range, and the return summary has
to be expressed in terms of those regions. That is the same
computation the region solver needs, which is the argument for doing it
once and properly rather than as a local patch.

The immediate consequence is that a growable container has no read-only
accessor yet, and its mutable accessors take `&mut Self`. That is
sound and it is what ships in the meantime; it is recorded in
`docs/spec/deferred.md` so the gap is visible rather than discovered.

## The gap produces wrong values, not just a worse API

A `Vec<T>` was written against these rules to find out how far a
container can get without them. Every operation is correct in
isolation: `push` grows and copies, `at_mut` bounds-checks, `pop`
returns the last element, and the destructor releases the buffer. But a
sequence of operations reads the wrong element:

```
mut v := Vec::<i32>::new()
v.push(1i32)
v.push(2i32)
v.push(3i32)
show("at2", if v.at_mut(2 as usize).is_none() { 1 } else { 0 })
last := v.pop()
show(last.unwrap())
```

The first conclusion drawn from this was that the container was blocked
here, on the reference model. **That was wrong.** Reducing the case
removed `Vec` altogether and left a much smaller fault:

```
enum E { A(i32), B }
fn mk(x: i32) -> E { ret E::A(x) }
fn get(e: E) -> i32 { ret match e { E::A(v) => v, E::B => 0 } }
fn sink(e: E) { _ := get(e) }
fn main() { e := mk(42i32); sink(mk(7i32)); sink(mk(9i32)); show(get(e)) }
```

`get(e)` yields `0`, not `42`. A callee builds an enum payload in its
own frame and returns a pointer to it, so the payload dangles as soon
as the call returns. What looks like a container bug is a returned
payload outliving the frame that made it, and it has nothing to do with
reborrowing. `docs/adr/0014-enum-payload-layout.md` records the layout
decision, and it is the real blocker: `Option<T>` is the enum every
container accessor returns.

The reference model is separately incomplete, and the reason a write
through a dereference goes unchecked is still worth stating plainly:

```
fn g(mut b: &mut i32) -> i32 {
  x := &*b      // a shared reborrow of the referent
  *b = 1        // a write through the same referent, not checked
  ret *x
}
```

This compiles and is unsound. The checker's `Place` is a root register
plus a field path, so a place reached *through* a reference has no
representation: a borrow of `*b` and a store to `*b` both resolve to
`b` itself. That is the missing machinery rule 1 and the region solver
describe, and it is why a container still has no read-only accessor. It
is a real gap, but a different one from the fault above.

## Consequences

- The unsoundness is understood and located, and the naive fix is known
  not to work, so the real work is not at risk of repeating it.
- A container is blocked on the enum payload layout rather than on the
  shape of its API. That is the first thing to fix, and it is smaller
  than the region work.
- The rules are recorded before implementation, so the coercions, the
  elision, and the extent computation are designed together rather than
  arriving as three patches that disagree.

## Alternatives considered

- **A lexical extent, shipped first as an approximation.** Rejected: it
  is sound but rejects correct programs, and the false rejections would
  be indistinguishable from real ones to the bootstrap. The design would
  also have to be thrown away rather than refined, since the extent is
  the solver's input.
- **Field-sensitive borrows.** Rejected in favour of whole-referent
  freezing, for the reason in rule 5.
- **Making the safe subset unable to express a systems program.** Not on
  the table; see `PRINCIPLES.md`.
