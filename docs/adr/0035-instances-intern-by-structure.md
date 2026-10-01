# ADR-0035: Instances Intern by Structure

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-02

## Context

`impl Iterator<(i32, i32)>` failed its own signature check, and a
struct field holding `(Maybe::Yes(1), 2)` mismatched its declared
`Holder<(Maybe<i32>, i32)>` with "expected 'enum', found 'enum'".
The instances are documented as "interned once and shared by
identity", but three places conspired to mint duplicates:

- Tuple element slots were the only slot copies without a recorded
  origin. Every other site — struct fields, variant payloads, the
  instance argument lists — copies through `storage_copy`, which
  records what a copy was made from; the two tuple sites copied raw.
- Instance lookup compared arguments by exact index. Tuple types are
  re-minted on each resolution (their element slots must be
  contiguous copies), so two spellings of one instantiation never
  shared an index.
- Structural equality normalized origins at the top level only. The
  comment claimed every level; nested copies compared raw and lost.

## Decision

**Slot copies record their origin, everywhere.** Both tuple sites —
annotation types and literal types — copy through `storage_copy`
like every other slot, so equality can see through the contiguity
copies to the shared originals.

**Instance lookup falls back to structural arguments.** The exact
index scan stays first and unchanged; on a miss, a second scan
compares arguments with `types_equal` before minting another
instance. Structurally equal arguments name the same type, so
returning the existing instance is correct by construction, and
every consumer — dispatch, coherence, monomorphization — keys off
the one canonical entry from then on.

**Equality normalizes at every level.** `types_equal_inner`
normalizes both sides to their origins on entry to each recursion,
which is what its comment always said. The seen-pair keys become
canonical with it, which only helps cycle detection.

## Consequences

What this buys is that one spelling means one type: tuple spec
arguments register, nested nominals unify, and the absurd
"'enum' versus 'enum'" mismatch is gone. Nothing downstream moves —
lookup, dispatch, and lowering already key off instance identity,
which is now true rather than aspirational.

What it costs is a structural scan per instantiation miss, linear in
instances times arguments; the exact-index fast path ahead of it
keeps the common case where it was. Duplicate tuple *nodes* are
still minted per resolution — the table holds structural twins whose
elements point home — because true table-level interning would need
structural comparison inside the builder, which owns no equality.
Memory only, and the same trade the slot copies already make.

What is explicitly out of scope: origin-recording the argument
lists stored on filled nominals, which lowering reads back only to
record — never to compare identity — and which dedupe keeps
canonical anyway; and any change to what equality means, which this
fix implements rather than extends.
