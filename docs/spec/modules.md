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
- Source paths use `/`; no `src/` hardcoding.
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
- Resolution order is lexical scope, then module, then (in future)
  the core prelude. Ambiguity is a compile-time error.

## Suites

A **suite** is a named set of packages. It is addressed as
`<owner>/<suite>` and a package within one as
`<owner>/<suite>/<package>`; a dependency entry is either form, and a
suite entry pulls every member. There are no suites in suites, so a
three-segment path is always a package.

**Every package is opt-in, `core` included.** A program that wants
`Option` asks for `alcy/std/core`. A name in scope then always traces
to a line in a manifest.

A package has two surfaces:

- `[modules] export` is the **public** surface: what `use` can reach.
  It is manifest-declared, per ADR-0007.
- The package's root module (`prelude.al`) is the **implicit** surface:
  its `pub` items need no `use`. This is a scope concern, not a way to
  hide an API, which is why it does not conflict with ADR-0007.

A suite guarantees that no name appears in two members' implicit
surfaces, so selecting a whole suite cannot produce an ambiguity.

The standard library is the `alcy/std` suite; see ADR-0016 for the
member list and the dependency graph.

## Packages

- A binary package declares exactly one `[[bin]]` target with an
  explicit `path` in `alcy.toml`; the target name defaults to the
  package name. A manifest without targets is an error; a directory
  without a manifest falls back to bare-directory mode.
- Library artifacts do not exist in MVP: path dependencies are
  source-included. `[lib]` targets arrive with summary-carrying
  artifacts, post-MVP.
- MVP compiles single-package programs only. Cross-package
  compilation follows the whole-program-analysis,
  per-package-emission model (see `deferred.md`).
- Symbol mangling for the alcy convention follows
  `alcy_<package>_<module path>_<name>`; `extern "C"` names are
  unmangled. Details finalize with package artifacts.
