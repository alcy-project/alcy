# Build guide

## Requirements

- GN, Ninja, Clang, LLD, libc++.
- Python 3.14+ managed by `uv` (`uv sync` once after cloning).
- On Linux/macOS, `nix develop` provides all of the above.

## Basic commands

```bash
# Debug build of the compiler:
uv run ./build/scripts/build.py --target=default --mode=debug

# Release build into a separate directory:
uv run ./build/scripts/build.py --target=default --mode=release --build-subdir=build_release

# Build and run the unit tests:
uv run ./build/scripts/run.py --target=tests --mode=debug

# All targets (compiler, tests, benchmarks):
uv run ./build/scripts/build.py --target=all --mode=release
```

Without `uv`, the shell wrappers `./build/scripts/build.sh` and
`./build/scripts/run.sh` accept the same arguments.

Build output goes to `out/<build-subdir>/` (`out/build/` by default).

## How the LLVM dependency works

The compiler links against a private LLVM fork, checked out as the
`third_party/llvm/src` submodule. At GN time the `//third_party/llvm:setup_llvm`
action prepares a ready-to-link installation under
`out/<build-subdir>/third_party/llvm/install/<debug|release>/` in this order:

1. **Reuse**: if the directory already exists and its recorded tag matches
   the submodule tag, nothing is done.
2. **Download**: otherwise a prebuilt archive for the submodule tag and the
   host triple is fetched from the fork's GitHub Releases
   (`llvm-<debug|release>-<triple>.tar.zst`, `.zip` on Windows) and extracted.
3. **Build from source** (only when `build_llvm=true`, not the CI default):
   `.alcy/configure.sh` configures a minimal static LLVM build and installs it.

The tag is resolved with `git describe --tags` in `third_party/llvm/src`,
so a shallow or tag-less checkout fails the setup. After submodule updates,
run `git -C third_party/llvm/src fetch --tags` if the download step reports
a missing asset.

## Platform notes

- **Linux/macOS**: libc++ is used (`-stdlib=libc++`); the `LIBCXX_*`
  environment variables point GN at its headers and libraries.
- **Windows**: the MSVC STL is used (no libc++). Both the compiler and the
  prebuilt LLVM libraries use the static C runtime (`/MT`, `/MTd`); mixing
  runtimes fails the link with
  `lld-link: error: /failifmismatch: mismatch detected for 'RuntimeLibrary'`.
  If you see this error, the LLVM installation was built with a different
  CRT — re-run the setup after removing the stale install directory.

## Troubleshooting

- `gn gen` failures: make sure `gn` and `ninja` are on `PATH`
  (`nix develop`, or install them via your package manager).
- Stale LLVM install: delete
  `out/<build-subdir>/third_party/llvm/install/` (and the tag cache at
  `build/scripts/llvm/.llvm_tag_cache`) to force re-resolution.
- Reference for all CMake flags of the LLVM build: run
  `cmake -N -L -S llvm -B /tmp/llvm-flags` in the fork, or read
  `llvm/docs/CMake.rst` there.
