# ADR-0085: A value is movable unless it is anchored

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

Every value today is movable: assignment, argument passing, and
`return` relocate a value bitwise and invalidate the old place. A
destructor makes a type move-only, which is the absence of `Copy` and
not the absence of a move (`docs/spec/values.md`). The drop pass reads
`Move` markers (ADR-0062); those record ownership transfers, not a
restriction on them. The two words are kept apart in this record:
move-only means "not implicitly copyable", and movable means "may be
relocated".

The pressure has arrived once already, in miniature. A closure's
environment is the closure's own address, so a closure that owns its
environment is self-referential, and the language refuses to move it;
`docs/backlog.md` records the intended fix, that the environment
belongs outside the value or beside the call as its own operand. That
fix is placement, not pinning.

Async and generators will produce values whose state machine holds a
borrow of itself across a suspension. There the value's storage is the
executor's, its address is its identity for the whole of a suspension,
and the type must be able to say that it is never relocated. No
placement fix applies: the value has nowhere else to live.

Rust is the counter-example. Movability was an axiom, and `Pin`,
`Unpin`, `PhantomPinned`, and projection boilerplate were the
retrofit; constructing the pinned invariant needs `unsafe`, and every
self-referential API repeats the projection shape. alcy is pre-MVP and
has no compatibility to keep, so the axiom is weakened before the
first value that needs it exists.

## Decision

**Move is always a bitwise relocation; `Movable` is a property the
compiler derives; and a value that contains an anchor is not movable,
is used through borrows, and ends in place.**

- A move copies the bytes and invalidates the old place. There is no
  user-defined move constructor and none is planned: a move never runs
  code, so it cannot be observed, and relocating a value is always the
  same work.
- **`Movable` is sealed and derived.** A type is `Movable` exactly
  when every field, array element, and variant payload is. A
  declaration cannot implement it or refuse it; the only way to lose
  it is to contain an anchor. It is not a bound the language asks a
  parameter to prove, and there is no negative spelling for it.
- **`Anchor` is the minimal unmovable value**: a compiler-known
  zero-sized type that is itself not `Movable`. The rule stays
  structural: a type that holds one in any field, element, or payload
  is not movable, and no separate marker case exists. The name is
  reserved for this role (Rust's `PhantomPinned` plays it there).
- An unmovable value is used through `&`/`&mut` only. It is created by
  an initializer that writes it directly into its place; it cannot be
  assigned over, passed or returned by value, swapped, or replaced,
  and it ends in place. A borrow of it is an ordinary movable value.
- **A type argument, parameter, and return is `Movable`.** Generic
  code that moves a value of a parameter type needs nothing extra,
  because every instantiation supplies a movable type, and an unmovable
  value is local to the scope that constructs it. This is ADR-0083's
  shape applied to a second property: a fact about every value a
  function can name, rather than a bound an author proves or relaxes.
- The placement rules that give an unmovable value its home — what an
  initializer guarantees, and whether a call or a `return` may
  construct into the caller's place — are reserved, not designed, until
  the first producer of unmovable values lands. The anchor exists now
  so the property has a spelling before it has users.
- An unmovable value with a destructor ends in place like any other
  value; the drop pass already ends values per place (ADR-0062). An
  anchor has nothing to end.
- **No `forget`.** The language has no API that leaks a value
  deliberately; a panic aborts and leaks what it owned, which stays
  the only leak (ADR-0013).
- `pin` is a reserved name for an operation that turns a movable value
  into an anchored place, should construction need one. Nothing ships
  it now.
- This record does not make owning closures unmovable. Their
  self-reference is fixed by placement, and a closure that owns its
  environment stays movable once the environment lives outside the
  value.
- The same derived-property machinery will carry the thread-safety
  properties when threads land, a type being safe to share or send
  unless it holds a marker. The guarantee's scope — threads, and not
  processes sharing mapped memory — belongs to that record, not this
  one.

## Consequences

- There is no `Pin`, no `Unpin` bound, no projection boilerplate, and
  no `unsafe` that upholds the property: it is derived, and giving the
  value its place is the compiler's job.
- The cost is placement. An unmovable value's initializer defines its
  address, so where such a value may be created must be stated before
  async lands. Until then the property is spellable — a struct with an
  `Anchor` field is unmovable — and testable.
- An unmovable value cannot be owned by `Box`, `Vec`, or a future
  `dyn` container: their construction moves a value in and their
  growth moves elements, and the type-argument rule refuses one.
  An aggregate that holds an unmovable value is itself unmovable and
  stays local. Owned unmovable storage is a decision of its own, the
  way owned unsized data is (ADR-0083); the executor that owns a
  future will need the answer, and it is reserved here rather than
  guessed.
- Generic code keeps moving its values, so the specialization pass
  (ADR-0079, ADR-0080) substitutes only movable types and stays total.

## Alternatives considered

- **Rust's `Pin`/`Unpin` retrofit.** Rejected: the property arrived as
  a wrapper over references, split the reference types in two, needed
  `unsafe` for construction, and turned every self-referential API
  into projection boilerplate.
- **Forbid self-reference with the borrow checker**, refusing a move
  of a value that holds a borrow of its own place. Rejected: the
  judgment is not local — aliases travel through calls, aggregates,
  and raw pointers — so it would refuse reachable programs and still
  miss unreachable ones.
- **Restructure everywhere, and never have an unmovable value.** This
  is the right fix for closures and is recorded as theirs; rejected as
  a universal rule, because an executor's future has no other storage.
- **A negative declaration** (`impl !Movable`), the `?Sized` shape.
  Rejected: the negative form is the regret. The opt-out is a field,
  not a declaration, and the property stays derived.
- **A user-defined move constructor**, so a move can fix up a
  self-reference. Rejected: it puts an implicit call in every move,
  makes relocation observable, and clears the one rule that makes
  moves cheap to reason about.
- **Make `Movable` an implicit bound on every parameter** the way
  `Sized` is in Rust. Rejected: an implicit bound is a fact every
  author must learn and every diagnostic must explain. Requiring it of
  type arguments and by-value passing — the places it is actually
  used — says the same thing without threading it through signatures.
