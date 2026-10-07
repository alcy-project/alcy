# pkg

Package model: the manifest and the toolchain file, and nothing a build
does with them.

- `manifest` parses `alcy.toml`-style manifests, package and suite
  alike. A package declares `name`, `version`, `license`, and an
  optional `owner`; a suite declares `name`, an optional `owner`,
  an optional `version`, `license`, and its member paths. Identity
  travels between manifests only when spelled: `version.suite = true`
  (and the same for `owner` and `license`) takes the suite's value,
  and `inherit_from_suite` fills those markers before
  `verify_manifest` runs (ADR-0057). Parsing reports through the bag;
  `verify_manifest` and `verify_suite_manifest` are the pure
  structural checks (names present and valid, a resolved version,
  targets well-formed, unique member paths and names) that every
  entry point runs before trusting a manifest.
- `version` and `version_req` parse strict `X.Y.Z` spellings and
  dependency requirements (`1.2.x`, `1.x`, comparisons, comma
  conjunction); the requirements expand to bounds at parse time.
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
  `base::Result<PackageManifest, diag::Reported>` and
  `parse_suite_manifest` for the suite shape.
- `verify_manifest(manifest)` ->
  `base::Result<void, ManifestError>` (pure) with
  `report_manifest_error` converting failures to bag diagnostics;
  `verify_suite_manifest` / `report_suite_error` are the same pair
  for suites.
- `inherit_from_suite(member, suite)` fills the `X.suite = true`
  keys; `probe_manifest_kind(bytes)` says which shape a file opens
  with, for a caller that has to choose a parser;
  `suite_member_name(path)` is the name a member path is addressed
  by.
- `parse_version_req(text)` -> `base::Result<VersionReq, VersionReqError>`,
  with each wildcard already expanded to the bounds it stands for.
- `parse_toolchain(bytes, bag)` -> `base::Result<Toolchain, diag::Reported>`.
- `parse_dependency_flag(bag, arena, "alcy/std/core")` parses one
  dependency the way the manifest grammar would, for `--deps`.

## Input requirements

- Never trust a parsed or caller-built manifest without
  `verify_manifest`: empty names, missing versions, unresolved
  inheritance, and malformed targets are rejected there, not at first
  use. `license` is required but may be empty, which is how a package
  grants nothing.
- A manifest that spells `X.suite = true` must have a suite to
  resolve against before verification; the caller that has the suite
  calls `inherit_from_suite` first and reports through
  `report_inherit_error`.
