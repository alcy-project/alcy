# ADR-0016: Suites, and the split of the standard library

- Status: accepted
- Date: 2026-09-27

## Context

`lib/std` is one directory with one file. The compiler embeds it and
injects the whole thing as a prelude module into every compilation, so
every program sees `Option`, `Vec`, `String` and the rest whether it
uses them or not. Rust gets the same effect from one crate plus
features; alcy would rather have real package boundaries, because a
boundary the dependency graph can see is a boundary the compiler can
check.

Two things make the single blob untenable. The analyzer hard-codes
which items are special by *name* — `is_core_fmt` accepts a callee named
`write` or `format` whose module is the prelude — so the compiler's
knowledge of the library is a string comparison. And `format` is not
lowered from its own body at all: `args: ()` is a placeholder that
erases the tuple's arity, so the expansion has to happen at the call
site, in a hand-written path that builds the value directly.

A third thing is missing rather than wrong: `[modules] export` is parsed
and structurally validated in `pkg/manifest.cc` and then never read, so
nothing enforces a public surface.

## Decision

### Suites

A **suite** is a named set of packages, addressed as
`<owner>/<suite>`. A package is addressed as `<owner>/<suite>/<package>`.
A dependency entry is either form: a suite entry pulls every member, a
package entry pulls one. A suite in a suite is not a thing, so a
three-segment path is always a package.

**Everything is opt-in, `core` included.** A program that wants `Option`
asks for `alcy/std/core`. Uniformity is worth the verbosity: a name in
scope always means a manifest asked for it, and the manifest is the
place to look.

### Two surfaces per package

- `[modules] export` is the **public** surface: what `use` can reach.
  It stays manifest-declared, per ADR-0007.
- `prelude.al` is the **implicit** surface: the package's root module
  re-exports the subset of its public names that need no `use`. It is a
  scope concern, not a way to hide an API, which is why it does not
  conflict with ADR-0007.

A package's implicit surface is a subset of its public surface. A suite
guarantees that no name appears in two members' implicit surfaces, so
selecting a whole suite cannot produce an ambiguity; that guarantee is
the thing `dependencies = ["alcy/std"]` buys over naming packages one by
one.

### The split

```
core    → nothing
alloc   → core
fmt     → core, alloc
arch    → core
simd    → core
atomic  → core, arch
thread  → core
sync    → core, thread
time    → core, arch
io      → core, alloc, arch
network → core, io
```

`MaybeUninit<T>` is a compiler builtin rather than a declaration, so
`alloc` needs core only for `Option` and `str`.

**`Vec` and `String` belong to `alloc`, not `core`.** They are
heap-backed, and the heap primitives they use are too. Leaving them in
core while moving the primitives to `alloc` would make `core` depend on
`alloc` and `alloc` depend on `core`. Moving them makes `core` the one
package with no dependencies at all.

`fmt` sits above `alloc` because `format` returns a `String`.

## Consequences

- The special-casing by name goes away. `write` and `format` are
  recognised as the `fmt` package's items, not as two spellings that
  happen to be called that in the prelude.
- A package's implicit surface is a deliberate list rather than
  everything it happens to declare, so `io` can export `File` for
  `use alcy/std/io::File` without putting `File` in every scope.
- `atomic`, `sync`, `io`, `network`, `thread`, `arch`, `simd` and
  `time` are declared but empty until they have content. An empty
  prelude contributes no names, so declaring them costs nothing and
  fixes the shape now rather than later.
- Every manifest, including a user's, names `core`.

## Staged landing

The compiler cannot yet select packages, resolve a suite, enforce
`export`, or resolve a `use` across packages — `Import` indexes one
module tree and MVP is single-package. So this lands in two steps.

**Landed here:** the layout, the manifests, the entry module per package,
and the ADR. The compiler still injects the whole suite, so behaviour is
unchanged and the implicit surface is every std item rather than the one
a package's `prelude.al` re-exports.

The nested prelude tree a real split needs has landed: staged sources
attach by slash-separated name, so a package's modules share a root and
a facade can reach them. Populating it does not work yet — staging
`core/prelude.al` alongside `core/mem.al` segfaults in the attach path
before analysis, rather than resolving the facade's `use mem::{...}`. So
each package is one entry module for now and the split is
organizational; `deferred.md` records the crash.

**Follow-up:** manifest suite resolution, per-package selection driven by
`dependencies`, enforcement of `export`, and cross-package `use`. Until
those land, `dependencies` is parsed and ignored, and the compiler is
more permissive than this ADR describes.

## Alternatives considered

- **One crate plus build features, as Rust does.** Rejected: a feature
  turns a dependency off inside a package that still contains the code,
  so the graph cannot see it. Real packages make the graph the unit.
  Features over packages are not excluded, only later.
- **A prelude file per package as the only surface.** Rejected: it
  cannot express "export this, but do not put it in scope", which is
  what keeps a dependency from becoming ambient.
- **`core` always implicit.** Rejected in favour of uniform opt-in: a
  name in scope then always traces to a manifest line.
- **Suites in suites.** Rejected: a three-segment path is already a
  package, and allowing nesting would make the arity of a path depend on
  a registry lookup.
