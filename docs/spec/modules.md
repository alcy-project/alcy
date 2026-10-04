# Modules, Packages, and Name Resolution (MVP)

## File and module mapping

- Modules are declared exclusively in `alcy.toml` under `[modules]`.
  `include` specifies module paths (e.g., `["main", "utils/io"]` or `["*"]`)
  mapped to files `main.al`, `utils/io.al`. The `mod` keyword is removed.
  Inline modules (`mod foo { ... }`) are no longer allowed.
- Name resolution runs after shadowing desugar (see `values.md`),
  which constrains pipeline ordering (desugar precedes resolution).
- Files not reachable from declared modules in `alcy.toml`
  produce a warning diagnostic, not an error.

## Module Resolution Design

- `mod` keyword removed; module declarations only in `alcy.toml`.
- `[modules]` table has `include` (list of paths or `["*"]`) and `export` (public API).
- Source paths use `/`; a module path never names a build directory.
- Module-to-file mapping is 1-to-1 explicit.

## Paths and imports

- The package root is addressed as `package::`; `self::` and `super::`
  address the current and parent modules. Dependency packages are
  addressed as `<package>::<path>`, where `<package>` is the
  `alcy.toml [package] name` with `-` normalized to `_`.
- `use a::b;`, `use a::b as c;`, and `pub use` re-exports are MVP.
  Glob imports (`::*`) are deferred.
- Default visibility is private; `pub` opens an item. There is no
  `priv` keyword. Restricted visibility (`pub(...)`) is deferred.
- Three namespaces exist: types, values, and modules. A struct name
  may denote both its type and its constructor expressions.
- Resolution order is lexical scope, then module, then the core
  prelude. Ambiguity is a compile-time error.

## Suites

A **suite** is a named set of packages, addressed as `<owner>/<suite>`
with a member at `<owner>/<suite>/<package>`. A dependency is a table
keyed by specifier, so the value carries a source: one segment is a local
directory to include, two name a package of an owner, and three name a
package in a suite. There are no suites in suites, so a three-segment
path is always a package. A whole suite is `<owner>/<suite>/*`; the bare
`<owner>/<suite>` is an error, because the two things it could mean are
one character apart. Globbing the suite and naming a member of it is
also an error: the selection would name the same package twice.

A suite specifier with `path` reads the suite manifest at that
directory instead of the embedded suite: a glob loads every member
the manifest lists, and a three-segment specifier loads the one
member it names, each through its own manifest. The manifest must
name the suite the specifier names, and a member the manifest does
not list is an error rather than an empty selection.

**Every package is opt-in, `core` included.** A program that wants
`Option` asks for `alcy/std/core`. A name in scope then always traces
to a line in a manifest. A selected package that needs another names it
in its own manifest, and a selection missing that edge is an error rather
than a silent addition.

`alcy compile` has no manifest, so it selects the whole suite by default
and takes `--no-std` to start from nothing, plus `--deps` in the same
grammar to name members or other packages.

A package has two surfaces:

- `[modules] export` is the **public** surface: what `use` can reach.
  It is manifest-declared, per `docs/adr/0007-manifest-driven-modules.md`.
- The package's root module (`prelude.al`) is the **implicit** surface:
  its `pub` items need no `use`. This is a scope concern, not a way to
  hide an API, which is why it does not conflict with `docs/adr/0007-manifest-driven-modules.md`.

A suite guarantees that no name appears in two members' implicit
surfaces, so selecting a whole suite cannot produce an ambiguity.

The standard library is the `alcy/std` suite; see `docs/adr/0016-suites-and-the-std-split.md` for the
member list and the dependency graph.

## Packages

- A binary package declares exactly one `[[bin]]` target with an
  explicit `path` in `alcy.toml`; the target name defaults to the
  package name. A library package declares one `[lib]` table instead,
  naming its root module the same way. A package holds at most one of
  each; a manifest with neither is an error, and so is a directory
  without a manifest: a directory is a package, and a package says
  which files it is.
- A library builds to a relocatable object with no entry: `main` is
  never required, and entry synthesis wraps one for binaries only.
  Summary-carrying artifacts arrive later; until then a lib artifact
  is an object file, linkable but opaque.
- Path dependencies load from source into the same closed world:
  the pipeline reads each dependency's manifest, stages its modules
  behind a fileless package root named by the manifest name, and
  checks everything together. A dependency addresses as
  `<package>::<path>`, where `<package>` is its manifest name with
  `-` normalized to `_`; a `use` or a qualified path that crosses
  the boundary reaches only the dependency's `[modules] export`
  list, and anything else across it is unresolved.
- MVP keeps emission per package: each build emits its root package
  only, with dependency code compiled in the way staged standard
  library sources already are. Summary-carrying artifacts and
  cross-package compilation follow the whole-program-analysis,
  per-package-emission model (see `deferred.md`).
- Symbol mangling for the alcy convention is per-signature and starts
  `_A`; the encoding is `docs/adr/0011-symbol-mangling.md`. `extern "C"`
  names are unmangled.
