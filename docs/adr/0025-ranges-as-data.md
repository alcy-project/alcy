# ADR-0025: Ranges as Data, Runs as Views

- Subject: the language
- Status: Accepted
- Date: 2026-10-01

## Context

Slices established that a view is a pointer and a length with the
buffer's loan, but nothing in the language names a run of one: `a[1]`
indexes an element and `&a` decays the whole array, with nothing in
between. Self-hosting needs the middle — diagnostics that quote a
source span, a lexer that re-slices its buffer — and the plan always
put it behind range expressions (`docs/spec/types.md`).

Two questions stood in the way. The first is what a range *is*: an
iterator in disguise, or data an index reads. The second is how an
index tells an element from a run without splitting the syntax, and
what owns the resulting view's loan.

## Decision

**A range is interval data.** `Range<T> { start: Bound<T>, end: Bound<T> }`
with `Bound = Included(T) | Excluded(T) | Unbounded`, both declared by
`core` as ordinary generic types, so a range expression constructs a
value that user code matches, passes, and stores like any other. No
iterator exists yet: iteration arrives with `spec` as an `Iterator`
implementation over a cursor type built from this interval (reached
through `into_iter`, per `docs/adr/0028-spec-system.md`), and stepping
lives on the cursor, never on the interval. A present start endpoint
is always `Included`; nothing in the language can exclude one.

Superseded in part by `docs/adr/0032-explicit-range-end-spelling.md`, which
requires a range with an end endpoint to spell it as `..<` or `..=`
instead of accepting bare `..` as a synonym of `..<`. Everything else
here stands.

**The names are reserved.** `Range` and `Bound` may only be declared
by the staged `core` package, the way `MaybeUninit` is compiler-owned.
A range expression therefore always constructs the one declaration,
and a program without `core` gets a diagnostic naming the package
instead of a silent fallback.

**An index dispatches on its position.** An integer position names an
element; a `Range<E>` position names the half-open run `[start, end)`
it decodes to, with both endpoints and their order checked at
runtime. A bare run of a fixed array is unsized, so only a borrow
names it (`&a[1..3]` → `&[T]`); re-slicing keeps the view's kind
(`&mut [T]` still writes); a `str` narrows to a `str`. A run is not
a place: assigning to one, or borrowing a view, is rejected where it
is written rather than in lowering.

**The borrow covers the container.** Lowering takes the element
address first and borrows second, exactly as for an element borrow,
so the view carries the array's loan and a write behind a live run
conflicts. Re-slicing extends the loan it was given rather than
taking a new one.

## Consequences

Range values materialize as the aggregate and enum the declaration
says: an `Unbounded` payload slot is never written, and the decoder
discards it with a `select` rather than a branch. Endpoint checks
precede the inclusion arithmetic, so neither the bounds nor the
`Included(MAX)` increment can wrap; an empty or inverted run panics
with "slice out of bounds".

Only integer ranges are indexable; float ranges are legal data with
no index meaning. The cursor that exposes integer ranges to `for`,
with the stepping it carries, and slice patterns remain the open
pieces. A `str` run extends whatever
loan the buffer carries, but views derived through `ExtractValue` on
a `str` drop it — the `str_slice` intrinsic has the same hole today —
so that case is tracked as a known gap in
`tools/check_borrow_rules.py` rather than as agreement.
