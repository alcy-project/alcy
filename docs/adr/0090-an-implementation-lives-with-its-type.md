# ADR-0090: An implementation lives where the type or the spec lives

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

`docs/spec/items.md` states that coherence is global — one
implementation per spec and type — and that a spec may be sealed to a
suite, with `[spec] implement` as the implementing package's opt-in
(ADR-0053). Nothing states *where* an implementation may live. As
written, any package that can name a spec and a type can add
`impl Spec for OtherPackagesType`, which makes a foreign type's
behavior a property of whichever packages a binary happens to link,
even though coherence keeps the set consistent.

The compiler-derived properties sidestep half of the problem already.
`Copy` is structural and opt-out-free, a destructor is a reserved
method, `Movable` is derived (ADR-0085), sizedness is an invariant
(ADR-0083), and the thread-safety properties will be derived the same
way. None of them is a spec a declaration implements, so the
"language built-in spec for a foreign type" case does not exist here.
What remains is the rule for declared specs.

Rust's orphan rule is the standard answer and carries its own regret:
"local type" needed covered types, `#[fundamental]`, and special cases
for references and boxes, and the resulting rule is one of the
best-known diagnostic puzzles in the language. alcy is pre-MVP and can
state a smaller rule.

## Decision

**An implementation lives where the type does or where the spec does:
`impl Spec for Type` is accepted in the package that declares `Type`
or the package that declares `Spec`, and nowhere else. The
compiler-derived properties are never implemented at all.**

- The two homes are the type's package and the spec's package. An
  inherent `impl` block has one home, the type's package, on the same
  rule.
- The spec's home is what makes a blanket implementation possible: a
  package that declares a spec may implement it for any type, which is
  where a blanket implementation belongs, and it is how `core`
  implements its own specs over its own types.
- The type's home is what a reader expects: the impls that concern a
  type live beside it, so a new implementation for a type cannot
  appear from a package the reader did not import.
- Sealing is orthogonal and stands: `[spec] suite-only` and
  `[spec] implement` decide *which packages may implement a sealed
  spec at all*, and this rule decides *where an implementation may
  live*. Both must hold, and a refusal names which one failed.
- **No covered types, no fundamental types.** `impl MySpec for
  Vec<MyType>` is accepted only where `MySpec` is declared; the type
  head (`Vec`) and its arguments do not make it local. A wrapper type
  is the answer when a foreign spec is wanted for a foreign compound,
  and the rule gains an extension only if a case earns one.
- The compiler-derived properties stay derived: there is no
  `impl Copy for X` (a destructor removes `Copy`), no negative
  implementation, and no way to implement sizedness or movability for
  a type the compiler did not derive them for.
- Global coherence stands: one implementation per spec and type. An
  implementation refused by this rule is refused where it is written,
  with a message naming the package that declares the type or the
  spec.

## Consequences

- The set of implementations that can affect a type is fixed by the
  type's package (plus specs the type's package chose to implement or
  that a spec's package supplied as a blanket). Library documentation
  can list a type's behavior exhaustively.
- A foreign spec over a foreign compound needs a newtype, which is a
  visible and cheap wrapper; the alternative is the covered-type
  machinery this record declines.
- The rule never grows a patch vocabulary: no `#[fundamental]`, no
  special cases for references or owning pointers; if one is ever
  needed, it is a decision with its own record.
- Diagnostics can say plainly why: the package declares neither the
  type nor the spec.

## Alternatives considered

- **Rust's orphan rule with covered types and `#[fundamental]`.**
  Rejected: the extension exists so that wrappers, references, and
  boxes compose, and it produced exactly the patch vocabulary this
  record exists to avoid. The simple rule plus the spec's own home
  covers the cases that matter.
- **No orphan rule: any package, with global coherence alone.**
  Rejected: the behavior of a foreign type would depend on the
  linked set, and two builds of the same source could see different
  implementations. Coherence would keep it consistent, not
  predictable.
- **Allow any package that names a type argument of the implemented
  type.** A covered-rule of one level. Rejected for now: it reopens
  the composition puzzle for a case a newtype handles today, and the
  rule should stay small until something proves it too small.
- **Keep the language properties as implementable specs** (`Copy`,
  `Drop`, `Send`-shaped). Rejected: it is the Rust shape that lets a
  declaration assert a property the type's structure contradicts, and
  ADR-0083/ADR-0085 already derive the properties instead.
