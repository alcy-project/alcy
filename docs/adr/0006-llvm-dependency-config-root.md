# ADR 0006: LLVM dependency managed via config.toml

- Status: Accepted
- Date: 2026-09-20
- Supersedes: ADR 0001, 0002

## Context

Previously, the LLVM dependency was managed via a Git submodule (`third_party/llvm/src`),
and the setup script resolved prebuilt release tags using `git describe --tags`
within the submodule (ADR 0002). A fallback to build LLVM from source via
CMake was also maintained.

This approach introduced several issues:
- Shallow checkouts or clones without tags caused `git describe` to fail,
  requiring manual `git fetch --tags` steps in local and CI environments.
- Maintaining the submodule checkout increased repository size and setup
  complexity unnecessarily, as standard development and CI never used source builds.
- Supporting source build scripts required CMake and complex CMake-invocation
  logic inside GN and Python scripts.

## Decision

We replace the submodule and source build fallback with a streamlined prebuilt-only
workflow centered around `config.toml`:

1. **Remove Submodule and Source Build**: Completely remove the `third_party/llvm/src`
  Git submodule, `build_llvm.py`, and CMake build scripts from this repository.
2. **`config.toml` as SSOT**: Define `llvm_fork_tag` in `config.toml` as the
  single source of truth for the LLVM dependency tag.
3. **GN and Script Integration**: Update `setup_llvm.py` and the GN action
  `//third_party/llvm:setup_llvm` to read `llvm_fork_tag` directly from
  `config.toml`. Track `//config.toml` in GN's `inputs` so that changing
  the tag in `config.toml` automatically triggers `setup_llvm.py` to fetch
  the new prebuilt archive from GitHub Releases.

## Consequences

What this buys:
- **Simpler repository and CI**: No submodule management, no `git fetch --tags`
  steps, and faster checkout times.
- **Removed CMake dependency**: Contributors no longer need CMake
  installed to build the compiler.
- **Single Source of Truth**: Updating LLVM requires changing only `llvm_fork_tag`
  in `config.toml`. GN/Ninja handles cache checking and downloads automatically.

What it costs:
- **No local source builds**: Fallback to building LLVM from source within
  this repo is no longer supported. Any new LLVM version or build variant
  must be built and published as a GitHub Release in `llvm-alcy-fork`
  before it can be consumed here.
