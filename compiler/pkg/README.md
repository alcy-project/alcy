# pkg

Package model: manifests, module discovery, dependency resolution,
and lockfiles.

- `manifest` parses `alcy.toml`-style manifests. Parsing reports
  through the bag; `verify_manifest` is the pure structural check
  (name present and valid, version parseable, targets well-formed)
  that every entry point runs before trusting a manifest.
- `modules` assigns source files to slash-separated module names.
- `resolve` locates the package root, loads the manifest, and
  resolves binary targets.
- `lock` converts resolved packages to lockfiles and serializes
  them, validating structure before writing.

## Entry points

- `parse_manifest(bytes, file, bag)` ->
  `base::Result<PackageManifest, diag::Reported>`.
- `verify_manifest(manifest)` ->
  `base::Result<void, ManifestError>` (pure) with
  `report_manifest_error` converting failures to bag diagnostics.
- `resolve_module_files`, `resolve_bin_target`, `lock_resolved` -
  each verifies incoming manifests at entry.
- `lock_resolved(packages, arena)` / `serialize_lockfile(lock, out)`
  -> `base::Result<_, LockError>`: validated before write, so a
  failed lock never half-writes the buffer.

## Input requirements

- Never trust a parsed or caller-built manifest without
  `verify_manifest`: empty names, bad versions, and malformed
  targets are rejected there, not at first use.
- Lockfile inputs are validated before serialization; on error the
  output buffer is left untouched.
