# ADR-0031: `for` Desugars in the Parser

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-01

## Context

ADR-0028 fixes the language: `for pat in head` iterates a cursor
named by `head.into_iter()`, that cursor must implement `Iterator`,
and the loop's `next` resolves through the spec, never through an
inherent method of the same name. The implementation has to add the
rule without teaching the analyzer or either lowerer a new node, and
without weakening inherent dispatch for ordinary calls.

## Decision

**The rule expands to existing AST nodes.** The parser rewrites
`for pat in head block` to one block expression:

    {
      mut _it := head.into_iter()
      loop {
        match _it.next() {
          Option::Some(pat) => block,
          Option::None => break,
        }
      }
    }

The declaration, loop, match, calls, and tuple-variant patterns are
the same nodes hand-written code builds, so verification, analysis,
lowering, and both back ends handle a `for` with no new cases. The
synthetic nodes carry the `for` keyword's span, which is where
diagnostics about the loop point.

**Spec-only dispatch for the generated call.** `ExprMethodCall`
gains `spec_only`; the desugar sets it on `_it.next()`, and method
lookup skips the inherent phases when it is set (`lookup_method`
splits its inherent scans into `lookup_inherent_method`). A head
whose cursor has an inherent `next` but no `Iterator` implementation
is therefore an error, and it gets its own message ("'for' requires
an 'Iterator' implementation in scope for this type") rather than the
generic unknown-method wording.

**`in` leaves the reserved set; `for` leaves its reserved
diagnostics.** The reserved-word recovery skips a token as the
cursor advances past the previous one, so a skipped `in` would vanish
before the header could read it; `for` was already a contextual
token for `impl` headers and reported reserved-use from item and
statement position. Both now parse in the rule and are ordinary
parse errors elsewhere. `keywords.md` and `grammar.ebnf` freeze the
move.

**Hygiene reuses alpha-renaming.** The cursor is spelled `_it`, and
the existing shadowing pass freshens nested loops, so two loops in
one function get distinct bindings and the item pattern may itself
spell `_it`.

**Parentheses lift the struct-literal ban.** A `for` head parses
under the condition rule (`{` opens the body), which left
`for x in (Ctor { field: 1 }) {}` unspellable: the `(` primary did
not lift the ban even though `grammar.md` told readers to
parenthesize. It now does, fixing the same case in `if`/`while`/
`match` conditions.

## Consequences

Both lowerers and the whole analysis pipeline receive the rule for
free: the only analyzer change is the lookup skip, and lowering is
untouched. Behavior is pinned by parser unit tests for the desugared
shape and the struct-literal lift, `ok-for`, `err-for-no-into-iter`,
`err-for-not-iterator`, and `err-for-pattern` for checking, and
`exe-for` for running — including the case where an inherent `next`
and an `Iterator` implementation disagree, which pins the dispatch
boundary at run time.

The desugar resolves `into_iter`, `next`, and the `Option` path in
the caller's lexical scope, so a local item shadowing any of those
names changes the loop; only the cursor binding gets hygiene. That
matches how the surface form names its pieces and keeps the rule
inspectable.

Refutable patterns are presently analyzed but rejected by the
lowerer, along with literal patterns nested in a payload, so `for`
binds the whole item; the filtering question is recorded as deferred
with the range cursor. Testing also surfaced a pre-existing gap:
generic instances intern by exact argument index, while tuple types
are re-minted on each resolution, so two spellings of one instance
(here `Iterator<(i32, i32)>` against its substituted return type)
compare unequal in the spec signature check. Nominal item types
avoid it, and the fix belongs with type-table interning rather than
this rule.
