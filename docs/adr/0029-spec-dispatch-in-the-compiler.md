# ADR-0029: Spec Dispatch in the Compiler

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-01

## Context

ADR-0028 fixes the language: specs declare signatures, impls supply
bodies per type, dispatch is static and scoped, coherence is global.
The implementation has to hang that on the existing method
machinery — inherent registration, lazy generic instantiation, and
the side tables lowering reads — without disturbing either, since
every inherent call must resolve exactly as before.

## Decision

**The AST gains one item and one field.** `ItemKind::Spec` carries
`ItemSpec`, whose methods are `SpecMethod` signature data rather
than items, so no pass visits them as bodies and verification
checks their types without a block to bound. `ItemImpl` gains the
implemented spec as a type path, invalid for inherent blocks; the
spec and target both parse through the closed-type grammar, and a
non-path spec side is rejected where the impl is written.

**`spec` and `for` lex distinctly but are never skipped.** After
the `comp` precedent, both have token kinds outside the reserved
set the cursor skips: `spec` matches in item position only, and
`for` in `impl` headers only, with the reserved-word diagnostic at
a stray item or statement. No identifier in the tree spells either,
so nothing else moves.

**Specs live in the type namespace.** `collect_locals` and the
prelude injection admit `Spec` items there, which makes `use`
imports and facade re-exports work unchanged. Registration rejects
a colliding type or spec name in the same module, duplicate
methods, and method-level type parameters, which wait for bounds.

**Spec methods are `MethodInfo` with an index.** The entry carries
the checker's spec-table index (`NO_SPEC` for inherent), so
lowering seeds and calls both kinds untouched: dispatch is fully
static and the impl block is not a runtime entity. The exact-match
phase skips spec entries, and a new phase runs after generic
inherent instantiation: it keys the receiver's nominal and
arguments, matches coherence records by the overlap rule, keeps
only the specs in scope at the call (plus the impl under check,
for sibling calls), and reports an ambiguity rather than choosing.
Destructors never come from specs: registration forces
`is_drop` off.

**Coherence is a record list.** Each impl appends its spec, target
shape, and spec-argument shapes; concrete arguments are types and
generic ones are bare impl-parameter names, so overlap needs no
solver. Concrete impls resolve both signatures beside each other
and register eagerly; generic impls check presence now and
signatures per instantiation through a mirror of
`instantiate_method`, with bodies checked under the substitution
and the spec pushed for sibling resolution.

**Diagnostics are catalog keys.** Eight analyzer keys cover the
new rejections (conflict, missing and unknown methods, signature
mismatch, type parameters, non-nominal and indirect targets,
ambiguity); the parser needs none, reusing its expected-item and
reserved-word wordings.

## Consequences

What this buys is the first slice with no lowering changes at
all: every behavior is analyzer-side, pinned by parser unit tests,
`exe-spec`, the `ok-spec` and `err-spec-*` cases, and the grammar
and keyword updates that the token additions require. Inherent
lookup is byte-for-byte the old behavior plus a skipped field.

What it costs is one more phase in the hottest lookup and a
second registration path to maintain beside inherent impls. The
overlap rule deliberately cannot see through nesting
(`Vec<Vec<T>>` is rejected, not unified), and method-level type
parameters stop at registration: both loosen when bounds arrive,
which also brings the `where` token and the struct/enum parameter
question this slice leaves alone.
