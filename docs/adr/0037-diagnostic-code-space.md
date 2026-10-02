# ADR-0037: A letter per component, and ids each component counts itself

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-02

## Context

A diagnostic code was a number, and the number was a stride per stage:
`lexer` owned 2000–2999, `parser` 3000–3999, `analyzer` 4000–4999, and
so on to 8000, with 9000 and up unassigned. `compiler/docs/diagnostics.md`
was the registry that restated the strides, and 70 constants lived in
twelve source files — four of them in `.cc` files where nothing outside
that file could see them.

Two things were wrong with it, and both came from the same place: the
number was doing two jobs.

It said which stage found the problem, and it said which check. Giving
each stage a fixed thousand meant a stage knew how many stages there were
and which order they ran in — a claim about the whole compiler made in
`lexer.cc`, in a literal, on the theory that nothing would ever change.
It also meant a stage had one thousand codes, spent in hundred-wide
sub-ranges, and the sub-ranges were narrower than the stages using them:
the analyzer's type checking had ten codes and had run out of room
before, in practice, the checks that were being added.

The thing that finished it was a second code generator. A native backend
alongside the LLVM one needs its own codes, and inside a stride that means
either splitting the stride or putting one component's codes in two
places. Neither is a decision anyone should have to make to add a
backend.

Meanwhile `docs/spec/grammar.md` had come to name `E3004`/`E4050`/`E5005`
— the specification naming the implementation, which is what
`tools/check_spec.py` exists to prevent, and which it could not see
because a bare code matched none of its four patterns.

## Decision

A code is a pair: which component, and which of that component's checks.
The letter is the component, and the id is the component's own, counted
from 1.

The letters were handed out in build-flow order, with the first letters
going to the components that report most, so that the codes a reader meets
most are the shortest. `A` is the lexer, which has six codes and will have
more when UTF-8 validation arrives; a component gaining its first error
does not renumber. `E`, `N` and `W` are held back from the component
alphabet because they are the severity letters and sit in the same
bracket, and a component holding one would render `EE`. A letter is
retired with its component and never reassigned.

`CodegenLlvm` and `CodegenNative` are two components rather than one,
because they are two implementations of one pipeline position with
different failure modes, and a code search that answered for both would
answer for neither.

`DiagCode` is an `enum class : u16` per component, in that component's own
header, listing its checks in the order they were assigned. `DiagBag::emit`
takes a `diag::Stage` and an enumerator, constrained to be an enum, so the
narrowing happens once inside `emit` and a bare number at a call site does
not compile. `ir` has no enum: its code is the `VerificationErrorKind`'s
ordinal plus one, so a kind and its code cannot drift apart.

`--json` reports a code as `{"stage": "analyzer", "local_id": 16}` rather
than as a string, so a tool never parses a letter. A message from outside
a check area keeps `"code": null`.

`compiler/docs/diagnostics.md` becomes an index: the letter table, and
what each check means. It no longer restates the numbers, and it drops the
twenty-four references to source files and symbols that were the part
most likely to rot. `tools/check_spec.py` gains a pattern for a
diagnostic code.

## Consequences

- A component knows its own ids and nothing else. Adding a component takes
  a letter, and adding a check takes the next id in one file.
- A duplicate code is a duplicate enumerator, which does not compile. That
  class of mistake was previously invisible: two checks could both emit
  `error[E4020]` and the registry would list 4020 once.
- Codes read `error[EC016]`, so a reader knows the provenance without the
  registry, and a tool can match a prefix per component.
- Ids are padded to three digits, so codes sort in assignment order, which
  is the order a bug report lists them in. The ceiling is 255, which is
  what a `u8` id gives; the widest component has 32, and a component near
  the ceiling wants its checks split rather than a wider field. Nothing is
  suppressed to get that: `performance-enum-size` asks for the smallest
  type the current value set needs, and here the current value set and the
  promised capacity are the same number.
- `Stage` is a set of components, not a list of pipeline positions, and two
  of the ten are alternatives. A run has exactly one of them.
- The registry can now disagree with the code, which it could not before.
  A commit that moves a code moves its line in the index too. Generating
  the index was considered and rejected: saying what a check means is not
  something a program can do, so a generator would only ever produce a
  list.
- The printed form is a breaking change for anything matching `E4\d{3}`.
  Pre-1.0, and the alternative was a scheme with no room for the second
  backend.
- `ir`'s fragility is unchanged: a code is still a kind's ordinal, so
  inserting a kind renumbers the kinds after it. The new scheme does not
  pretend otherwise.
