# ADR-0142: A place path names its nest and the match consumes once

- Subject: the compiler
- Status: Accepted
- Date: 2026-11-16

## Context

The ownership journal names every place an event touches as a local
plus a path of constant steps, and the drop walk, the borrow pass, and
the ends that consult the journal all read the same path back. The
model's first form covered fields and tuple slots only: a constant
index `GetElementPtr` was one step. An enum payload names its place
with `EnumFieldAddr` instead, which no path covered, so a payload
binding recorded no move event and the walk's state for the payload
stayed `Owned`. An end that consults the journal then ran a payload's
destructor on a buffer a binding owns, which is the double free the
match-binding cases pinned.

Separately, a match's own consumption marker and the part markers its
arms place collide on the same shell: the arms' in-place ends mark the
fields moved, and the whole-scrutinee marker the match places at its
join reads them as a second move. A flag on that one marker was the
shape that kept a user's whole-after-partial move refusing while the
match's own bookkeeping folded.

## Decision

A place's path is one flat array of `u32` steps and every nest encodes
into it the same way: a constant-index `GetElementPtr` appends one
step, and an `EnumFieldAddr` appends two, the variant first and the
payload field second. `place_of_register` resolves both instruction
forms onto the same path array, the drop walk's enum branch ends a
payload at that same two-step path, and every journal query answers
against the array, so a payload's move event and the ends that read it
agree without a per-nest encoding.

A move's `InstructionFlags` carry `subsumes_parts`, set only by the
match's join marker. The borrow pass removes the entries a flagged move
subsumes, a strict prefix, and leaves every other move's check alone:
a whole move a user wrote after a part move still conflicts, and the
part's destructor still ends what the user gave up. Because the flag is
part of the serialized instruction form, the binary format's major
bumped to 6; a reader that predates the bit refuses the file rather
than guessing at a flag it never read.

## Consequences

An end that consults the journal skips what a binding moved out,
whether the binding sits in a field, a tuple slot, or an enum payload,
so a payload's destructor runs once, from the end that owns it. The
borrow pass trusts the flag: lowering sets it only at the match's join,
and a review that finds a second setter has found a bug. The two-step
enum path means a nested payload's path grows by two per enum level,
which is why the path stays an array rather than a packed field; the
ends that walk payloads construct the child paths by appending, and the
journal queries compare prefixes, so the growth is bounded by the
nesting depth of the type.
