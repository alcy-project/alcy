# ADR-0060: The path vocabulary is three forms

- Subject: the language
- Status: Accepted
- Date: 2026-10-09

## Context

A path's head can be `package` (the package root), `self` (the current
module), `super` (the parent module), a dependency's identity, or a
child module of the current module. Four of those are relative
spellings of one form and one is absolute, so the resolver carries a
parent walk for `super` alone.

That walk is the wart. A parent reference depends on a module's depth
(`super::super::x`), so moving a module to another level rewrites every
path that climbs out of it; the resolver needs a "root module has no
parent" edge; and `super` is a keyword whose only job is to make the
walk shorter than the absolute spelling that always works. Rust keeps
`super` and pays for it; Python made absolute the default and kept
explicit relative dots. alcy has no users yet, so nothing is bound to
either choice.

`crate::` is the same kind of habit from another language, but it is
not a keyword: it fails as an unresolved name, which is a worse report
than a targeted one.

## Decision

A path names its target in exactly one of three ways:

1. **`package::...`** — absolute in the package root. It reaches every
   module of the package and is stable under moves; a prelude's
   re-exports read `pub use package::cmp::Eq;`.
2. **A relative head** — a child module of the current module, or, with
   `self::`, the current module itself. `self::` stays: it is the
   explicit spelling of the relative form, needed to reach a local
   module whose name a dependency also claims (a dependency wins the
   bare head) and to re-export a single-segment item (`use` needs two
   segments).
3. **A dependency's identity** — `<package>::...`, absolute in that
   package, unchanged.

`super` leaves the vocabulary and the keyword set. It is an ordinary
identifier; a path whose head is `super` and that fails to resolve gets
a targeted message naming `package::` instead of the generic unresolved
one. `crate` is not a keyword at all and gets the same shaped message,
because the two are the same mistake from neighboring languages.

The guidance is a resolution-time check, not a reservation: if a module
or dependency literally named `super` exists, the path resolves and no
message fires. There is no error-only keyword left for this, which is
the point — the language has no users, so there is nothing to migrate
and no reason to keep a dead keyword.

Reserved words for syntax that will exist (`async`, `await`, `union`,
`register`, `where`, `dyn`) are a different thing and stay reserved;
ADR-0061 records that line.

## Consequences

- The standard library's preludes move 39 `pub use super::child::item;`
  re-exports to `pub use package::child::item;`. Nothing else in the
  tree spells `super`.
- Resolution has no parent walk: every upward reference is absolute,
  and `super` disappears from the lexer, the parser's path heads, and
  the tree-sitter grammar's `path` rule.
- `super` and `crate` are usable as ordinary identifiers, including as
  module names; the guidance fires only where a path head fails.
- The diagnostic is deletable: once the habit is gone, the check is one
  string comparison at a failure site and can be removed without
  touching the grammar.

## Alternatives considered

- **Keep `super`.** Rejected: it is the only depth-dependent spelling
  in the vocabulary, and `package::` already expresses everything it
  does.
- **Keep `super` but forbid chains.** Rejected: a rule that says "one
  level up but not two" is a puzzle, not a simplification.
- **Keep `super` reserved but rejected.** Rejected: with no users,
  reservation buys nothing and leaves dead surface area (the same
  reasoning that keeps the future words reserved applies only to
  syntax that will exist).
- **A leading `::` or `crate::` alias for the root.** Rejected: the
  root has one spelling, `package::`; two spellings for one thing is
  the ambiguity `self::`/`package::` already has to explain.
