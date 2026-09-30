# analyzer

Module resolution (`resolve.h`) and type checking (`types.h`).

- `resolve_modules` lexes, parses, and desugars every file, builds
  the `ModuleTree`, and resolves imports. Module membership comes
  from the caller, never from source items, so no new files enter
  the compilation. Value and type expressions are NOT resolved here.
- `check_package` resolves every type position to interned `TypeIdx`
  and checks bodies, producing a `CheckedPackage`.

`Checker` (in `checker.h`, included only by the analyzer's own
translation units) carries the checking state; it is not public API.

## Entry points

- `resolve_modules(root, modules, package_name, sources, ast, bag,
  prelude)` -> `base::Result<ModuleTree, diag::Reported>`. Unknown
  file ids are rejected (`ANALYZER_INVALID_PATH`); failures leave the
  tree unusable and the bag holds the diagnostics.
- `verify_module_tree(tree)` ->
  `base::Result<void, ModuleTreeError>`: non-empty, root in range,
  no null modules, prelude count within range. Pure.
- `check_package(tree, width, ast, bag)` ->
  `base::Result<CheckedPackage, diag::Reported>`. Validates the tree
  (`ANALYZER_INVALID_MODULE_TREE`) and the arena before any pass runs,
  so hand-built trees fail with a diagnostic instead of UB.

## Input requirements

- `CheckedPackage::types` is proof-carrying `ir::VerifiedStorage`:
  verified when the checker built it. Lowering reseeds from the
  proof instead of re-verifying.
- Pointer width is passed explicitly, never sniffed from the host.
