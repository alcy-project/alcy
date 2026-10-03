# ADR-0016: Suites, and the split of the standard library

- Subject: the language
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
  It stays manifest-declared, per `docs/adr/0007-manifest-driven-modules.md`.
- `prelude.al` is the **implicit** surface: the package's root module
  re-exports the subset of its public names that need no `use`. It is a
  scope concern, not a way to hide an API, which is why it does not
  conflict with `docs/adr/0007-manifest-driven-modules.md`.

A package's implicit surface is a subset of its public surface. A suite
guarantees that no name appears in two members' implicit surfaces, so
selecting a whole suite cannot produce an ambiguity; that guarantee is
the thing `"alcy/std/*" = {}` buys over naming packages one by one.

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

Selection, staging, and the facades landed in steps; `export`
enforcement and a `use` across packages have not, because `Import`
indexes one module tree and MVP is single-package.

**Landed here:** the layout, the manifests, the entry module per package,
and the ADR.

**Landed since:** staged sources attach by slash-separated name, so a
package's modules share a root and a facade reaches them; each package is
split into modules behind its `prelude.al`. Selection is manifest-driven:
`[dependencies]` names members or the whole suite, the closure is required
rather than pulled in, and only the selected members are staged — a
program sees exactly what its manifest names. The suite manifests are the
single source of truth: the embed script validates them and generates the
tables, so a manifest edit needs no script edit beside it. An unresolved
name that a member carries names that member, which is what makes an
opt-in `core` diagnosable rather than mysterious. `alcy compile` selects
the same way, with `--no-std` and `--deps`, since a single file has no
manifest to select from. The staged bytes are copied into the source
manager under their suite-relative names rather than written to a scratch
directory and mapped: nothing reaches the filesystem, two compilations
cannot read each other's prelude, and a diagnostic about a standard
library source names `core/prelude.al` instead of a temporary path.

**Follow-up:** enforcement of `export` for standard-library members,
cross-package `use` between members, and the fetchers every other
owner and suite needs. Path dependencies already trim to their export
lists; until the standard library's lists trim too the compiler is more
permissive than this ADR describes for its own members: an unselected
member is absent, but a selected one is not yet trimmed to its
`export` list.

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
