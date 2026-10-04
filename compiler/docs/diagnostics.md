# Diagnostic codes

A code identifies a check: the letter says which component of the
compiler found it, and the number says which of that component's checks.
It reads `error[EC016]` — error, analyzer, check 16 — and it is what a
tool matches on, what a bug report quotes, and what `--json` carries as
`{"stage": "analyzer", "local_id": 16}` so a reader never has to parse a
letter.

## Why a letter and not a number

The numbering used to be a stride per stage: `lexer` owned 2000–2999,
`parser` 3000–3999, and so on to 8000. Two things were wrong with it.

A stride is a claim about the whole compiler that a stage has no business
making. `lexer.cc` writing `2000` says that there are eight stages and
that lexer is the second; both of those were true, and both were the
lexer's business only until a ninth stage arrived. A letter says the same
thing and is stable: `A` is the lexer whether there are eight stages or
twenty.

And a stride runs out. The backend is the case that forced it — a native
code generator alongside the LLVM one needs its own codes, and a second
codegen stage inside one stride means either splitting the stride or
putting a component's codes in two places. Two components that are
alternative implementations of one position get two letters, because a
code search that answered for both would answer for neither.

## The letters

Assigned in build-flow order, and the flow is what a reader needs to know
a code's provenance, so `compiler/docs/architecture.md` carries the same
order. The first letters go to the components that report most, and a
component that gains its first error does not renumber: `A` is the lexer
partly because UTF-8 validation is coming and it should not have to move.

| Letter | Component | Directory | Checks |
| --- | --- | --- | --- |
| `A` | lexer | `compiler/lexer` | 6 |
| `B` | parser | `compiler/parser` | 8 |
| `C` | analyzer | `compiler/analyzer` | 35 |
| `D` | lowering | `compiler/lowering` | 7 |
| `F` | borrow | `compiler/borrow` | 4 |
| `G` | ir | `compiler/ir` | one per `VerificationErrorKind` |
| `H` | pkg | `compiler/pkg` | 4 |
| `I` | pipeline | `compiler/pipeline` | 16 |
| `J` | codegen_llvm | `compiler/codegen_llvm` | reserved |
| `K` | codegen | reserved for the native backend | reserved |

`E`, `N` and `W` are not in the table because they are the severity
letters and they sit in the same bracket; a component holding one of them
would render `EE`. A letter is retired with its component and is never
reassigned, because a code in a bug report has to mean the same thing for
as long as the report can survive.

## The ids

Each component counts its own from 1. Zero is never a check, so a
diagnostic with no code is not a diagnostic with code zero. The width is
fixed at three digits so codes sort in the order they were assigned,
which is the order a report lists them in. 255 is the ceiling, which is
what a `u8` id gives, and the widest component has 35; a component near
the ceiling wants its checks split rather than a wider field.

`ir` is the one component with no enum of its own. A code there is the
`VerificationErrorKind`'s ordinal shifted by one, so a kind and its code
cannot drift apart; adding a kind needs no renumbering, and the message
is the kind name.

## What this document is

An index. The number a check has lives in its component's
`diag_code.h`, and this file says what the check means — so the two can
disagree, and a commit that moves a code moves its line here too. The
alternative, generating this from the enums, is a tool that has to run
before every commit to be worth having; saying what a check means is not
something a program can do.

`diag::Code` is a pair and not a number, so nothing in the compiler can
do arithmetic on a code or invent one: `DiagBag::emit` takes a component
and an enumerator, and the enumerator is what a call site names.

## Notes on the numbering that is gone

The old registry also carried two facts that belong to the code rather
than to the prose, and both are now enforced by the compiler rather than
by this file. A duplicate code was a table row that happened to match two
checks; it is now a duplicate enumerator, which does not compile. A code
in the wrong stride was invisible; the stride is a letter and a
component's ids are its own, so there is nothing to get wrong.
