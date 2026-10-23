# ADR-0084: A string is text, and text sits above the allocator

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

ADR-0016 placed `String` in `alloc` and `Vec`, `Map`, and `Set` in
`collections`, which made `core` the one package with no dependencies
and left `alloc` needing core only for `Option` and `str`. That split
answered what each package needs from *memory*, and it is the reason
the base stays clean today.

Since then, `String`'s method set grew past memory. It carries
`read_file` and `write_file`, which are I/O, and the string API
decided alongside this record gives it `count_graphemes`, which is
table-driven Unicode. Neither belongs to the package whose job is the
allocator and the owning pointers. Leaving the methods where they are
and `String` in `alloc` would make `alloc` name `io` and `unicode`,
and would pull Unicode tables into every program that links the
allocator, including the freestanding ones. ADR-0074 used the same
reasoning for the hash: a module's home follows what it must name.

The string also has a property its neighbors do not: `str` is a
compiler-known view in `core`, so the byte-level operations
(`len_bytes`, `count_chars`) need no package above `core` at all, and
only the table-driven ones need a layer above. The placement can say
exactly that.

## Decision

**`String` moves to a new `text` package above `alloc`; table-driven
Unicode moves to a new `unicode` package above `core` only; `Vec`
joins `alloc`; and `alloc` keeps `core` as its only dependency.**

- `alloc` is the allocator and the raw buffers: `heap`, `Box`, and
  `Vec`. It depends on `core` only, as ADR-0016 intended. `String`
  leaves it, and the I/O helpers leave `String` for `io`.
- `unicode` is the table-driven algorithms over `str`:
  `count_graphemes`, `graphemes`, `width`, and case, allocation-free
  and depending on `core` only. A freestanding program gets grapheme
  counting without the allocator.
- `text` is owned text: `String`, with `len_bytes` and `count_chars`
  delegating to `core`'s `str` and `count_graphemes` delegating to
  `unicode`. It depends on `core`, `alloc`, and `unicode`.
- `collections` is the structures with lookup algorithms, `Map` and
  `Set`, above `alloc`.
- The preludes stay the user-facing surface: `text/prelude`
  re-exports `String`, and `collections/prelude` keeps re-exporting
  `Vec` from `alloc`, so a program reads the same after the move and
  only the package paths change.
- This amends ADR-0016's placement. The line is now drawn by what a
  package must know: memory and raw buffers in `alloc`, text above it,
  tables above `core`, lookup structures in `collections`.
- A program that names neither `text` nor `unicode` stages neither,
  so the tables are reachable only from the packages that asked for
  them.

## Consequences

- `alloc` stays the base it is: core only, no text, no I/O, no tables.
- The string API can be symmetric. `String` offers `len_bytes`,
  `count_chars`, and `count_graphemes` because its package sits above
  both `core` and `unicode`; `str` alone reaches the first two from
  `core`.
- Moving the string changes package paths, not the prelude surface.
  `alloc/prelude` stops re-exporting `String`; a manifest that named
  `alloc` for the string names `text` or the suite.
- `io` gains `text` as a dependency, and `read_file` and `write_file`
  live there as free functions rather than methods on `String`.
- Two new packages join the suite's member list. ADR-0016's rule makes
  that cheap for consumers: naming a member requires its closure
  rather than pulling names in, and only the selected members are
  staged.

## Alternatives considered

- **`String` stays in `alloc` and `alloc` depends on `unicode`.**
  Rejected: the allocator package would name text semantics, every
  allocator user would stage the tables, and ADR-0016's own principle
  of a dependency-light base erodes.
- **`String` moves to `collections`.** Rejected: a string is not a
  lookup structure. `collections` would own text semantics, and a
  program that only wants `Map` would carry `String`'s Unicode.
- **One new package for text and unicode.** Rejected: `unicode` needs
  only `core` while `String` needs `alloc`, so one package either
  makes grapheme counting require the allocator or forces every
  `String` user to stage tables a freestanding string program should
  not need. The two-layer split is the fact being recorded.
- **Unicode tables in `core`.** Rejected: `core` is the language
  primitives, and every program would stage tables it may never use.
  The explicit package list is what lets the tables have a home only
  their users name.
- **`Vec` stays in `collections`.** Not chosen. `Vec` is a raw buffer
  like `Box`, and putting it in `alloc` makes the line single: buffers
  in `alloc`, structures in `collections`. Keeping it below `text`
  also means the string work needs no edge into `collections`.
