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
