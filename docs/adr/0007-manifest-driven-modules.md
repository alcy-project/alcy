# ADR-0007: Manifest-Driven Module Resolution (Removal of `mod` Keyword)

- Status: Accepted
- Date: 2026-09-21

## Context

The language previously required `mod foo;` or `mod foo { ... }` declarations inside source files to declare modules. This duplicated structural information already present in the package directory layout and manifest (`alcy.toml`), violating the principle of explicit behavior and increasing compiler implicit knowledge. We required a design that eliminates the `mod` keyword entirely, keeps source-to-module mapping explicit, and allows flexible project structures without hardcoding `src/` directories.

## Decision

1. **Remove `mod` keyword and `ModItem` from the language.** All module declarations are eliminated from source files.
2. **Module definition lives exclusively in `alcy.toml`** under the `[modules]` table:
   - `include = ["*"]` or explicit path list (`["main", "utils/io"]`) defines build targets.
   - `export = ["..."]` defines the public API surface for library packages.
3. **File-to-module mapping is 1-to-1 and path-based:** module name `foo/bar` maps to file `foo/bar.al` relative to the manifest. No implicit file scanning except via explicit wildcard `*` in `include`.
4. **No `src/` pre-knowledge:** package root is the source root. Source file placement is flexible; only `alcy.toml` defines what is compiled.
5. **Module visibility is manifest-controlled:** external exports are declared in `alcy.toml`, not via `pub use` hidden in `lib.al` or source files.

## Consequences

- Source files are pure logic; module boundaries are build-level policies.
- `parser`, `ast`, `lexer`, and `analyzer` no longer handle `ModItem`, simplifying the pipeline.
- `alcy new` can use `modules = ["*"]` to create quick starter projects, while production packages use explicit lists.
- To fully complete this, `pipeline` and `pkg` must implement manifest parsing of `[modules]` and file-to-module resolution. The `resolve_modules` analyzer path has been disabled for inline module construction; it must be rebuilt around manifest input rather than AST `ModItem` scanning.
