# pkg

Package model: manifests, dependency resolution, and lockfiles.

- `manifest` parses `alcy.toml`-style manifests. Parsing reports
  through the bag; `verify_manifest` is the pure structural check
  (name present and valid, version parseable, targets well-formed)
  that every entry point runs before trusting a manifest.
- `resolve` resolves the package's path dependencies: one `ResolvedPackage`
  per manifest it loads, depth-first and cycle-checked. Only tests call it
  so far, because a build does not load dependencies yet.
- `lock` converts resolved packages to lockfiles and serializes
  them, validating structure before writing. Written into a buffer, not a
  file: lockfiles stay out of builds.

Which files a manifest makes modules is `pipeline`'s
(`pipeline/select_modules`), because that is a question about what
discovery found rather than about the manifest.

## Entry points

- `parse_manifest(bytes, file, bag)` ->
  `base::Result<PackageManifest, diag::Reported>`.
- `verify_manifest(manifest)` ->
  `base::Result<void, ManifestError>` (pure) with
  `report_manifest_error` converting failures to bag diagnostics.
- `parse_toolchain(bytes, bag)` -> `base::Result<Toolchain, diag::Reported>`;
  the bytes are the caller's, so nothing here opens a file.
- `lock_resolved(packages, arena)` / `serialize_lockfile(lock, out)`
  -> `base::Result<_, LockError>`: validated before write, so a
  failed lock never half-writes the buffer.

## Input requirements

- Never trust a parsed or caller-built manifest without
  `verify_manifest`: empty names, bad versions, and malformed
  targets are rejected there, not at first use.
- Lockfile inputs are validated before serialization; on error the
  output buffer is left untouched.
