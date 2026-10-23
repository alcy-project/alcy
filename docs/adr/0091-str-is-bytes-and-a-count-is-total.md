# ADR-0091: `str` is bytes, and a count is total

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

`docs/spec/types.md` says `str` is a byte sequence backing string
literals and that MVP performs no validation. The naming record
(ADR-0088) reserves `count_chars`, `count_graphemes`, `char`,
`chars()`, and `graphemes()`, and the layering record (ADR-0084) puts
the counts above `core`'s bytes. Before those land, what a count
answers for an arbitrary byte sequence has to be stated, or the counts
inherit an undefined edge.

Rust chose the other side: `str` is always valid UTF-8 and `[u8]` is
the byte type, so constructing an invalid `str` needs `unsafe`. That
makes decoding total, and it makes the invariant a promise every
boundary — files, FFI, `unsafe` — must either keep or break on
purpose. alcy's spec says bytes; the counts must therefore answer for
every byte sequence, and the scalar type's invariant must be checked
where scalars are made.

`char` (ADR-0088) means a Unicode scalar value: not a byte, not a
surrogate, not a grapheme. Deciding where a `char` can be produced
decides whether that invariant is maintained by construction or by
convention.

## Decision

**`str` is a byte sequence with no validity invariant, every count is
total, and `char` is a scalar value that only a checked decode
produces.**

- Nothing validates `str`: not a literal, not `String`'s byte APIs,
  not a foreign function's bytes. The type promises bytes, and the
  byte view — `len_bytes`, `byte`, `byte_slice` — is the whole of its
  core API.
- `count_chars` and `count_graphemes` are total. Each walks the bytes
  and follows Unicode's substitution rule: an ill-formed subsequence
  counts as one replacement unit, so every byte sequence has a count.
  The same rule governs decoding when it lands; it is stated once,
  here, so the count and the walk cannot disagree.
- `unicode::utf8::valid(str) -> bool` is the predicate for a program
  that requires validity. It is a query, not a gate: the counts do not
  depend on it, and neither does any other core operation.
- `char` is a Unicode scalar value, and a checked decode is the only
  thing that produces one: the bytes decide whether a scalar exists at
  that position, and the API that decodes chooses between the lossy
  reading (a replacement unit) and a failure (its own spelling, when
  one is wanted). A `char` is never ill-formed, so a `char`-walking
  iterator walks the lossy stream, and its name says it walks.
- The counts are counts of encoding units, not of bytes:
  `len_bytes` is O(1) over the bytes, `count_chars` and
  `count_graphemes` are O(n), and their names carry the cost (ADR-0088
  rule 3).
- Grapheme segmentation (UAX #29) and width stay deferred; the names
  are reserved, and when they land they operate on the same lossy
  scalar stream this record defines.
- Literal validation is not introduced by this record. Whether a
  source literal is checked is a strictness decision about the source,
  and no count needs it to be total; the record reserves that
  decision.

## Consequences

- A program can hold bytes that are not UTF-8 in a `str` or a
  `String` deliberately, and moving bytes in and out needs no
  `unsafe`: what the bytes are is not a type-system promise, so
  nothing has to be hidden to keep one.
- Validity is decided in exactly one place, `utf8::valid`, and every
  decode states which reading it takes. The compiler never rejects a
  program for its bytes.
- `len_bytes` stays the only constant-time length. A caller who wants
  one of the other counts pays for a walk and sees that in the name.
- The substitution rule is testable with byte escapes, so invalid
  sequences pin the count behavior the same way a valid one does.
- A later strictness decision about literals can land without
  changing a count, because the counts already answer for invalid
  input.

## Alternatives considered

- **Rust's validity invariant** (`str` is always valid UTF-8;
  `[u8]` is the byte type). Not chosen: it turns `str` into a promise
  every boundary must keep, so bytes from a file, a foreign function,
  or an audit need validation or `unsafe`, and the counts only have to
  be total because the invariant can be broken from outside the
  language. alcy's byte view says what a string is on a system, and
  validity stays a query anyone can make.
- **A count refuses invalid input** (an error, or a panic).
  Rejected: counting is used where failing is worse than counting —
  sizing a display, reporting a position — and validity has its own
  predicate for callers who need it.
- **Count one unit per invalid byte.** Rejected: the substitution
  rule is Unicode's reading of an ill-formed sequence and the one
  decoding will use; two rules for the same bytes would drift.
- **Validate literals now.** Not chosen: strictness about the source
  is a decision of its own, and deferring it keeps a program that
  spells raw bytes compiling while the counts are already total.
