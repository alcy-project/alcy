# ADR-0034: Ranges Iterate Through a Cursor

- Subject: the language
- Status: Accepted
- Date: 2026-10-03

## Context

ADR-0028 promised that a range exposes its iterator without being
one: `into_iter` returns the cursor, and the interval stays data.
ADR-0032 then fixed how a range end is spelled, and what a startless
or endless range means as data — but not what either means as a loop.
Three questions stood in the way: where iteration starts when no
start is written, what an inverted range yields, and whether an open
end terminates. The implementation also has to land as ordinary
standard-library code, with no compiler support beyond what `for`
and the spec system already provide.

## Decision

**The cursor is `RangeIter<T>`, built by `Range::into_iter`.** Both
are generic over the element type and live in core beside the
interval, re-exported through the prelude. The cursor holds the next
value to yield, the end it stops at, and whether it is spent; a
single `impl<T> Iterator<T> for RangeIter<T>` serves every integer
width, since the stepping logic never names a width.

**A startless range fails; an endless one does not end.** A range
with no start has no first value, so `into_iter` reports an error
rather than guessing a minimum — the unsigned minimum is obvious
but the signed one is not, and ten per-width spellings of it would
be. A range with no end yields until the loop stops it, like any
counter the body breaks out of. An inverted or empty range yields
nothing.

**The inclusive end never steps past itself.** The cursor reports
the end as its last value and marks itself spent instead of
advancing, so `0..=255u8` never computes `255 + 1`. The check is per
call, not per width, which is what makes one generic implementation
correct for all of them.

**Matches stay on owned `Self`.** The analyzer resolves a variant
pattern against a generic enum only through the implementation's own
`Self` — never through a field or a reference of generic enum type —
so the cursor never matches on its bounds directly. `Bound` answers
three predicates (`first`, `admits`, `is_last`) by matching on
itself, and the cursor asks them. The predicates stay module-private;
the public surface is the interval, the cursor, and `into_iter`.

**An open range reads as a `for` head.** The `{` after the head
always opens the loop body, so `for i in 0..` takes the open range
rather than reading the body as a block-valued end. Parentheses
still lift the ban for a range that genuinely ends in a block.

## Consequences

What this buys is the last open piece of the range work: `for i in
0..<n` and `for i in 0..=n` run, over every integer width, with the
only per-width behavior — where the maximum stops — falling out of
the generic rule. The range remains plain data everywhere else:
indexing, sub-slicing, and matching over intervals move nothing.

What it costs is one deliberate asymmetry: a startless range is
fine as data (an index reads it from its container's start) but an
error as a loop, and an endless range is an obligation on the loop
body to stop. Both are stated in the specification rather than left
to the wrapping arithmetic to imply.

What is explicitly out of scope: strided iteration, which the
cursor does not offer; descending ranges, which stay empty rather
than yielding downward; and matching on generic enum values beyond
`Self`, which stays a compiler limitation this design routes around
rather than fixing.
