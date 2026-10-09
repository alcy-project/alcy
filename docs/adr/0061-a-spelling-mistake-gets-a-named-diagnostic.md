# ADR-0061: A spelling mistake gets a named diagnostic

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-09

## Context

A compiler sees the habits of the languages its users came from. A
Rust programmer writes `let a = 1`, a C programmer writes `i32 a = 1`,
and both currently get `Expected '=', found 'a'` or `Expected path,
found 'i32'` — reports that name a token, not the line the writer
meant. An undeclared `a = 1` gets `Unresolved value 'a'`, which hides
that the fix is `:=`. And `super::x` / `crate::x` get a generic
unresolved name (ADR-0060 retires the first and names the second).

The parser already has two precedents. `ParserCompDeclarationSpelling`
reads `comp x := ...` as the const declaration it should have been and
reports the spelling once, so no cascade follows; `RangeEndUnspelled`
is a named code for one shape. The reserved-word path is a different
thing: `async` and friends are read past because the syntax *will*
exist, and the reservation keeps those spellings free.

## Decision

A **spelling diagnostic** recognizes a tight token shape at the
earliest point that can decide it and then parses the line as the
construct the neighboring language meant, so the run reports exactly
one diagnostic and no cascade. Two family codes carry them: parser
`EB009 Spelling` and analyzer `EC043 SpellingSuggestion`. The message
names the alcy spelling; the code is family-level because the shapes
are one class of mistake, the way six reserved words share one code.

A shape fires only when the line cannot be valid alcy:

- `let` / `var` followed by a pattern (`let = 1` reassigns a binding
  named `let` and is untouched).
- `mut <name> =` (a declaration cannot start with `mut` and end in
  `=`).
- `<name>: T =` (an ascription is a declaration, never a place).
- a primitive type followed by `<name> =` (a type is never a place).
- `return` followed by an expression (a call to a binding named
  `return` has `(` next and is untouched).
- an assignment whose single-segment place resolves to no local, item,
  or import at all.

Phase 1 lands those six. Phase 2 adds `if let`/`while let`, item
keywords (`func`, `trait`, `import`, `mod`), macro-call `name!(...)`,
and `&self` parameters. Phase 3, if it ever lands, covers `class`,
`switch`, `++`, C casts, `extern crate`, glob imports, and attribute
`#[...]`.

Reserved words for syntax that will exist (`async`, `await`, `union`,
`register`, `where`, `dyn`) stay reserved; converting them to ordinary
identifiers would let user code claim spellings the future syntax
needs. Spelling diagnostics are for constructs that will *not* exist
here, which is why `super` was converted (ADR-0060) and `where` was
not.

## Consequences

- The message teaches the spelling instead of naming a token; the code
  is greppable in tests (`EB009`, `EC043`).
- The parser reads the line as the declaration it meant, so a fix at
  the reported span is the only edit needed.
- The `comp` declaration spelling moves from `UnexpectedToken` to the
  new `Spelling` family, since it is the same class of mistake.
- The shapes are a maintained list; a new one needs a test that pins
  both the message and the single-diagnostic property, and the parse
  tests keep the valid programs the shapes must not touch.

## Alternatives considered

- **A general "did you mean" suggester over names.** Rejected: edit
  distance finds a misspelled identifier, not a whole construct
  spelled another language's way; shape knowledge is what makes these
  messages exact.
- **Reserve `super` and `crate` as keywords instead of diagnosing
  them.** Rejected: with no users, a dead keyword buys nothing and
  blocks two ordinary names (ADR-0060).
- **Leave the generic errors.** Rejected: the token-level message is
  the compiler telling the writer about its own grammar at the moment
  it could instead say what to write.
