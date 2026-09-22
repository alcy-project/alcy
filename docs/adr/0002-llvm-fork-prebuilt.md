# ADR 0002: Private LLVM fork consumed as prebuilt libraries

- Status: Superseded by ADR-0006
- Date: 2026-09-16

## Context

The compiler needs a fixed set of LLVM libraries with a stable configuration
(static libs, no RTTI/EH, all targets) across Linux, macOS, and Windows.
Building LLVM from source on every developer machine and CI job is slow and
fragile, while upstream LLVM releases do not match the required configuration.

## Decision

Maintain a private fork (`llvm-alcy-fork`, no source changes, build
configuration only in `.alcy/` and `.github/alcy-release.yaml`), check it out
as the `third_party/llvm/src` submodule, and consume it as prebuilt static
libraries downloaded from the fork's GitHub Releases, keyed by submodule tag
and host triple. A from-source build remains as a fallback
(`build_llvm=true`), and the GN `setup_llvm` action reuses, downloads, or
builds in that order.

## Consequences

- Fast, reproducible LLVM provisioning; the exact LLVM revision is pinned by
  the submodule pointer.
- The fork must publish release archives for every supported triple and build
  type, with a build configuration (notably CRT and libc++ selection)
  matching the compiler's flags - otherwise the link fails (see
  `docs/build.md`, platform notes).
- Updating LLVM means cutting a fork release and bumping the submodule.
