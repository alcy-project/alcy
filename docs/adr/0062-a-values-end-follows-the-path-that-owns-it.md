# ADR-0062: A value's end follows the path that owns it

- Subject: the language
- Status: Accepted
- Date: 2026-10-11

## Context

`Move` markers say where a value leaves a place. Placing the ends -
the destructor calls - is lowering's job today, and lowering decides
it with a flag per local and a one-bit latch that records "some path
moved this". The latch is too coarse, and the runtime guard it
produces is the wrong shape for the borrow checker:

- An `if` whose arms differ in whether they move a value leaves the
  guard, and the guard's own load and move read as a use after a
  maybe-move. `if c { take(r) }` and `Option::filter` over a non-Copy
  `T` are rejected though both are sound. The per-arm epilogue
  machinery that would end the value on the arm that still owns it
  exists, but it computes its delta after the arm's flags are
  restored, so it never fires.
- A store re-owns a place, but it does not clear `moved`, so
  `take(r); r = Res::new()` leaks the second value.
- A destructor reached through an enum variant or an array element
  needs a discriminant read or a loop, which the emitter does not
  build, so such values leak - warned about when a binding is
  involved, silent on a wildcard match. `Vec`, `Map`, and `Set`
  release their buffers without ending the live elements for the same
  reason: library code has no way to ask for one end.
- Discarding a value with a destructor is an error, so the two
  spellings of a discarded value disagree - one errors, one warns -
  and neither ends the value.

ADR-0013 chose the destructor's shape: a by-value receiver, one
destructor per type, and scope exit placing the call. What it left
open is where the call goes.

## Decision

**Ownership is a per-place dataflow over the CFG.**

A place is a local, or a field path from one, and its state is owned
or moved. A move sets moved; a store that writes a whole place, or one
field of it, re-owns what it writes. The join of owned and moved is
maybe, and maybe is a property of the program point, not of a path:
where the paths differ is exactly where an end has to be placed on
some of them and not on others.

**Ends are placed by a pass over IR, per CFG path.**

Lowering stops placing ends. It emits the `Move` markers and stores it
already emits, and journals, per function, an end site for each place
that goes out of scope (a block exit, `ret`, `break`, `continue`, and
`?`), with the place, its block, and its position in that block. The
pass is its own translation unit (a `drop` module beside lowering); as
each function's body finishes and before the function is registered,
it reads the journal, the markers, and the stores, solves the dataflow
per place, and materializes each site:

- every path reaching the site owns the place: the end is appended at
  the site;
- every path moved it: nothing is appended;
- the paths differ but each edge has a definite state: the edges are
  split, and the end goes on the ones that own (a new block on the
  edge, branching to the original target);
- a state that is still maybe - a loop that can carry either state to
  the point - is decided at run time by a flag allocated per place.
  The flag is set where a move happens and cleared where a store
  re-owns.

The pass rewrites a block by appending a fresh, consecutive copy of
its instruction list with the ends inserted and re-pointing the block
at it. The storage stays append-only, every range stays consecutive,
and the verifier eats the result unchanged; instruction spans follow
the copies. Appending blocks extends the function's range because the
function is registered only after the pass is done.

**The `Drop` instruction marks a placed end.**

The compiler emits `Drop(place)` before the end's own load and move.
The borrow checker consumes it as an authoritative ownership boundary:
at that instruction the place is owned, so the end's move is not a use
after a move, whatever the may-move analysis says about the paths that
reach it. That is the fact a guarded end needs, and it is what retires
the false positives above. Codegen and the interpreter already ignore
the opcode; it stops being reserved and starts meaning something.

**Ending a value ends what it holds.**

The own `drop` runs when declared. Otherwise the compiler walks the
fields the value is made of: a struct's fields, the live variant's
destructible fields after a discriminant read, and an array's elements
in a loop. The warning that asks a type to declare its own `drop` for
a variant or an array retires, and a wildcard match that discards a
payload ends it on the way out.

**Library code can ask for one end.**

An unsafe intrinsic `drop_in_place<T>(p: *mut T)` runs the same glue
for the value at `p`, resolved per instantiation the way `size_of` is.
`Vec`, `Map`, and `Set` end their live elements in `clear` and `drop`
through it. A `Drop` spec stays the direction ADR-0028 records - one
coherence-unique impl the compiler calls as glue - and waits for
bounds, which are a later item.

**Discarding a value ends it.**

`_ := x` runs the glue where it stands, and a bare expression
statement whose value is not `()` warns and does the same. The
`LowerDiscardedDestructor` error retires; one rule replaces the two
spellings.

## Consequences

- The cases the latch rejected now check: an `if` that consumes on one
  arm, `Option::filter` over a non-Copy `T`, and a loop that consumes
  and re-owns.
- A drop flag survives only where a loop carries both states to one
  point; an end every path owns is a direct call, and paths that
  differ split at their edges. Placement lives in one pass, so
  `break`, `continue`, `?`, and joins share one rule.
- The borrow checker keeps its move analysis. The `Drop` marker is the
  one fact it lacked, and ADR-0012's region model is untouched.
- The pass costs one rewrite per function that needs it, and must
  produce the same bytes at every job count (ADR-0048): its block and
  flag order are the lowering order, never a hash order.
- A value behind a borrow still cannot be ended; that is the affine
  rule, unchanged.
- The variant/array and partial-move entries leave
  `docs/spec/deferred.md` as their slices land, and ADR-0013's
  placement section is amended.

## Staged landing

- S1. The journal and the pass: direct ends and the flag for a maybe;
  lowering's flag, guard, and epilogue placement retires. Acceptance
  cases: a conditional move, and `Option::filter` over a non-Copy `T`.
  Splitting an edge whose paths differ, so the common joins need no
  flag at all, is the pass's next refinement.
- S2. Re-owning stores and flag resets; the `Drop` marker and its
  consumption by the borrow checker. Acceptance cases: a loop that
  consumes and re-owns, and the guarded end of a loop-carried maybe.
- S3. Enum payload and array element glue. A wildcard match discard
  still moves the scrutinee without ending the payload it leaves
  behind; that lands with the discard rule.
- S4. `drop_in_place`, and `Vec`/`Map`/`Set` ending live elements in
  `clear` and `drop`.
- S5. The discard rule: `_ := x` and a bare statement both end the
  value, with the error retired, and a wildcard binding of a droppable
  place ends what it discards.
