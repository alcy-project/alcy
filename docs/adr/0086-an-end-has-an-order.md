# ADR-0086: An end has an order, and no value is forgotten

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

`docs/spec/values.md` says a scope exit runs the destructors of the
values its block declared, innermost first, and a `return` runs the
destructors of everything still alive. Neither says in what order the
values of one block end, nor in what order a value's own parts end.
ADR-0013's drop glue says the compiler ends each destructible field of
a type without a destructor, and ADR-0062's pass places every end per
place, but the order those ends run in is a property of the language,
and it is observable through destructor effects.

The order matters for two reasons. A later local may borrow an earlier
one — `a := ...; b := &a;` — so ending the borrowed value first would
let its destructor run while a reference to it is still alive. And a
partially-ending aggregate is only comprehensible if the order is
stated once, at the declaration, rather than left to the placement
pass.

Rust is the near-miss this record picks up after the fact: it fixed
field order late (declaration order, matching the source) and local
order (reverse declaration), and the split surprises authors; its
`mem::forget` and `ManuallyDrop` are the escape hatches callers reach
for when a destructor should not run, and they make every destructor
side effect unprovable. alcy's drop pass proves where ends go; a leak
API would make that proof conditional.

## Decision

**An end has a stated order — a block's values end last-declared
first, and a value's parts end in declaration order — and there is no
API that forgets a value.**

- A scope exit ends the block's values in reverse declaration order:
  the value declared last ends first. Shadowing declares a fresh
  binding, and each follows the same rule. This keeps a borrower's end
  ahead of the end of what it borrows.
- A `return` takes the result first, then ends the values still alive
  in the same reverse order. ADR-0013 already takes the result first,
  for the same reason.
- A struct's or tuple's fields end in declaration order, an enum's
  live payload ends in declaration order, and an array's elements end
  in index order. A type with its own destructor runs it first; the
  fields of its receiver end when the destructor's scope exits, in
  the same order.
- The std's sequences end their elements from first to last; a
  container's own destructor states its element order where it is
  written.
- **No `forget` and no `ManuallyDrop`-shaped wrapper.** A value's
  destructor runs at its end, or the program panics and leaves what it
  owned to the process — the one leak the language has (ADR-0013).
  Deliberately leaking a value has no spelling, so "a value ends" is a
  fact the drop pass can keep proving.
- An unmovable value's parts end in the same order, in place
  (ADR-0085).
- Destructors do not run on the panic path, as recorded; this record
  does not change that.

## Consequences

- The order is observable only through destructor effects. Work
  without such effects may still be reordered or eliminated; the ends
  themselves may not.
- A destructor can rely on the order: what it borrows of its own
  parts is still alive until its scope exits, and what it holds in a
  later-declared field ends after an earlier one.
- A type that must not run its destructor on a path has to express
  that with ownership — moving the value to where its end belongs —
  rather than by skipping the end. The panic path remains the only
  way not to run one, and it aborts.
- The drop pass's placement (ADR-0062) now has an order to follow,
  and a test can state the order a generated journal must show.

## Alternatives considered

- **One rule: reverse declaration order everywhere, fields
  included.** Simpler to state, and rejected because a field order
  that does not read the way the declaration does is a surprise for
  no gain: fields cannot borrow each other, so nothing forces the
  reverse, and declaration order is what a reader of the type already
  assumes. Locals are the case where the reverse is forced by
  borrows.
- **No stated order; leave it to the implementation.** Rejected:
  destructors have side effects, so the order is observable, and
  leaving it unstated freezes whatever the first implementation
  happens to do as the language.
- **A `forget`/`ManuallyDrop` API.** Rejected: it makes every
  destructor side effect conditional, gives callers a way to hide an
  ownership mistake instead of fixing it, and the only leak the
  language needs is the panic path's.
- **Run destructors on the panic path.** Rejected elsewhere: the
  program aborts, so there is no caller to return to, and running
  arbitrary destructor code while unwinding is where unwinding's
  complexity lives.
