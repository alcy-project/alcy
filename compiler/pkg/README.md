# pkg

Package model: the manifest and the toolchain file, and nothing a build
does with them.

- `manifest` parses `alcy.toml`-style manifests. Parsing reports
  through the bag; `verify_manifest` is the pure structural check
  (name present and valid, version parseable, targets well-formed)
  that every entry point runs before trusting a manifest.
- `toolchain` parses `.alcy/toolchain.toml`: which driver links a
  binary and what arguments it is given.

Nothing here opens a file, walks a directory, or decides which sources
are modules. The bytes are the caller's, which is what keeps the parser
testable without a filesystem:

- discovery and module selection are `pipeline`
  (`pipeline/select_modules`), because both are questions about what the
  run found rather than about the manifest;
- loading a manifest or a toolchain file is `pipeline`
  (`require_package_manifest`, `load_toolchain`).

Path dependencies load from source (`pipeline/dependencies`):
a build resolves the package it was pointed at and every path
dependency its manifest names, each read through its own manifest
and staged behind its own package root, which is
`docs/adr/0038-library-packages-loaded-by-source.md`. Lockfiles
are not implemented: the models for them were written and removed
rather than left tested-but-unreachable; they come back with the
fetcher that uses them.

## Entry points

- `parse_manifest(bytes, file, bag)` ->
  `base::Result<PackageManifest, diag::Reported>`.
- `verify_manifest(manifest)` ->
  `base::Result<void, ManifestError>` (pure) with
  `report_manifest_error` converting failures to bag diagnostics.
- `parse_toolchain(bytes, bag)` -> `base::Result<Toolchain, diag::Reported>`.
- `parse_dependency_flag(bag, arena, "alcy/std/core")` parses one
  dependency the way the manifest grammar would, for `--deps`.

## Input requirements

- Never trust a parsed or caller-built manifest without
  `verify_manifest`: empty names, bad versions, and malformed
  targets are rejected there, not at first use.
