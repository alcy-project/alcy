# ADR-0053: Operators are sealed specs

- Subject: the language
- Status: Accepted
- Date: 2026-10-07

## Context

ADR-0028 fixed the spec system's scope and left operator overloading
out of it, and ADR-0024 postponed `m[key]` until a spec could express
it. Both deferrals now block the standard library: `Vec` and `Map`
index only through named methods, and no type outside the builtin set
can say what `==` means. The constraint from ADR-0028 stands: an
operator in user code must not become an implicit function call,
because inference cannot carry the burden a hidden call places on the
reader.

The standard library is the one place where that constraint does not
apply: its types are the language's own vocabulary, and the compiler
already recognizes its packages by identity (`write`/`format`, the
intrinsic set, the runtime). What keeps that honest is the intrinsic
arrangement: the compiler owns the table, and the declarations are
verified against it rather than trusted.

## Decision

**An operator names a spec.** `a[i]` dispatches to `Index`, `a[i] = v`
and `a[i] op= v` to `IndexMut`, and `==`/`!=` to `PartialEq`, with
`!=` the negation of `eq`. A builtin operand keeps the builtin rule -
integers, floats, arrays, slices, pointers, and `str` are unchanged -
and the spec covers the nominal types: `Vec<T>`, `String`, and
`Map<V>`. Indexing through the spec panics out of bounds exactly as
arrays do. The compiler keeps one canonical table from operator
spelling to spec and method, and verifies the standard library's
declarations against it the way it verifies intrinsic shapes, so a
declaration and the operator cannot drift.

**A spec is sealed by its package's manifest.** A `[spec]` table
names the specs only the declaring suite may implement:

```toml
[spec]
suite-only = ["Index", "IndexMut", "PartialEq", "Eq"]
```

The list lives in the manifest of the package that declares the
specs, and the checker verifies every name resolves to a spec that
package declares; an unknown key inside the table is an error, so a
policy typo cannot silently drop the seal. An implementation outside
the declaring package's suite is a diagnostic that names the reserved
implementing-side key - `implement = [...]`, spelling not yet fixed -
which is not accepted yet. Specs a table does not name stay open, so
users keep implementing their own specs and standard-library specs
like `Iterator` for their own types; the operator specs are declared
in `core` and implemented by `alloc`'s types within the same suite.

**A spec may name a super-spec.** `spec Eq: PartialEq {}` says that
implementing `Eq` requires an implementation of `PartialEq`, and a
bound on `Eq` admits `PartialEq`'s method. `Eq` is the reflexive
guarantee and needs no method of its own. `Ord: PartialOrd` takes the
same shape when ordering ships.

**The first operator set is indexing and equality.** `Index<I, O>`,
`IndexMut<I, O>`, `PartialEq`, and `Eq`, implemented for the scalar
types and the standard containers. Floats implement `PartialEq` only:
IEEE equality is not reflexive, so `f32` and `f64` are comparable but
not `Eq`. A container that needs a reflexive key bounds `Eq`; ADR-0024's
`Map<V>` keeps its `str` keys until spec bounds land, at which point a
`HashMap<K, V>` bounds `Eq` and `Hash`.

Deferred with their own decisions: arithmetic and comparison
operators, `Display` for `{}`, a deref operator, and the
implementing-side manifest key that would let user packages implement
these specs.

## Consequences

`m[key]`, `v[i]`, and `==` on standard types arrive without a general
overloading mechanism, and the compiler's operator knowledge is a
checked table rather than a set of exceptions. The standard library
gains impls in the package that owns each type, and the checker gains
super-spec implication and the manifest seal, which every later spec
reuses.

What waits: `HashMap<K, V>` and generic algorithms bound on `Eq`,
because bounds are the spec system's last slice; and user-defined
operators, which return to the record as a reserved manifest form
rather than an open question.
