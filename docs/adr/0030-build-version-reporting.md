# ADR-0030: The Build Version is One Generated Translation Unit

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-01

## Context

`--version` has to identify the build a bug report came from, and the
build is a git commit. The value was a `-D` on the whole `cli` source
set (`ALCY_PROJECT_VERSION`), so a change to it invalidated every cli
translation unit and forced a full relink on each commit, even though
one string had moved.

The value also has to distinguish a release from a development build,
and it must not claim a clean commit when the working tree is modified:
during development the tree is almost always dirty, so a bare commit id
would describe the last commit rather than the binary that was built.

## Decision

The version is computed from the git state at `gn gen` time by
`tools/version.py`:

- On a clean `v*` tag: `<base> (<short-commit>)`, for example
  `0.1.0 (53871170f08f)`.
- Otherwise: `<base>-snapshot (<short-commit>)`, with `-dirty` appended
  to the commit when tracked files are modified, for example
  `0.1.0-snapshot (53871170f08f-dirty)`.
- Without a git checkout: `<base>-snapshot`.

`<base>` is `project_version` in `build/config/BUILDCONFIG.gn`, bumped
by hand at each release. The short commit is twelve hex digits.

The string is baked into a single generated translation unit,
`$target_gen_dir/version.cc`, produced by `write_file` and exposed
through `cli::alcy_version()` in `compiler/cli/version.h`. Only that
unit's compile command carries the commit, so a new commit recompiles
one file and relinks. `write_file` leaves the timestamp alone when the
string is unchanged, so a rebuild between commits does nothing.

## Consequences

A new commit costs one small recompile plus the relink, which is
inherent to an embedded version. The dirty marker changes only when the
tree crosses between clean and dirty, so active development does not
relink on every edit; a content hash of the working tree would, and is
out of scope.

`gn gen` runs the script through `exec_script`, so the toolchain needs
`git` at configure time; `flake.nix` provides it. A plain `ninja` does
not re-run `gn gen`, so the version updates on the next build through
`tools/build.py`, which always regenerates.
