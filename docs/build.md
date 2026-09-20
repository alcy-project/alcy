# Build guide

## Requirements

- GN, Ninja, Clang, LLD, libc++.
- Python 3.14+ managed by `uv` (`uv sync` once after cloning).
- On Linux/macOS, `nix develop` provides all of the above.
- A submodule checkout including tags: the LLVM setup resolves its prebuilt
  archive with `git describe --tags` inside `third_party/llvm/src`, so
  shallow or tag-less checkouts fail the setup. After cloning or updating
  submodules, run:

  ```bash
  git -C third_party/llvm/src fetch --tags --depth=1
  ```

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

`default` builds the compiler group only; test and benchmark executables
are not referenced by any group, so they build solely under `all` (or when
named explicitly, e.g. `--target=tests`).
For a complete list of supported flags and options, pass `--help` to any script.

Build output goes to `out/<build-subdir>/` (`out/build/` by default).

## How the LLVM dependency works

The compiler links against a private LLVM fork, checked out as the
`third_party/llvm/src` submodule. At GN time the `//third_party/llvm:setup_llvm`
action prepares a ready-to-link installation under
`out/<build-subdir>/third_party/llvm/install/<debug|release>/` in this order:

1. **Reuse**: if the directory already exists and its recorded tag matches
   the submodule tag, nothing is done.
2. **Download**: otherwise a prebuilt archive for the submodule tag and the
   target triple is fetched from the fork's GitHub Releases
   (`llvm-<debug|release>-<triple>.tar.zst`, `.zip` on Windows) and extracted.
   For wasm builds the triple is `wasm32-unknown-emscripten`; if the fork has
   not published that asset yet, the setup fails — publish it first (see the
   fork's `alcy-release.yaml`).
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
- **WebAssembly**: built with Emscripten (`emcc`/`em++` on `PATH`) targeting
  `wasm32-unknown-emscripten`, using Emscripten's bundled libc++ (the
  `libcxx` GN config is excluded). Always use a dedicated output directory
  (e.g. `--build-subdir=build_wasm --target-os=emscripten`); reusing a native
  output directory leaves stale artifacts behind. Test binaries run under
  `node` (`run.py` handles this automatically).

### Windows CRT details

The static CRT is a deliberate cross-repo contract, enforced on both sides:

- The fork builds LLVM with `CMAKE_MSVC_RUNTIME_LIBRARY` set from the build
  type (`MultiThreaded` / `MultiThreadedDebug`) in `.alcy/configure.sh`.
- This project passes `-fms-runtime-lib=static[_debug]` in
  `build/config/compiler/BUILD.gn`.

One asymmetry is worked around rather than fixed upstream: Clang's
`-fms-runtime-lib=*_debug` still selects the *release* CRT (`libcmt.lib`)
instead of the debug CRT (`libcmtd.lib`), leaving debug-only symbols such as
`_malloc_dbg` unresolved. Until that driver bug is fixed, Windows Debug
builds additionally pass `-Xlinker /NODEFAULTLIB:libcmt.lib -Xlinker
/DEFAULTLIB:libcmtd.lib` (same file, `linker` config). Do not remove those
flags without re-checking the driver behavior.

To verify which CRT an artifact uses, inspect its directives, e.g.
`llvm-readobj --coff-directives <lib>.lib | grep RuntimeLibrary` should show
`MT_StaticRelease` / `MTd_StaticDebug` for alcy objects and LLVM libraries
alike.

## Troubleshooting

- `gn gen` failures: make sure `gn` and `ninja` are on `PATH`
  (`nix develop`, or install them via your package manager).
- Stale LLVM install: delete
  `out/<build-subdir>/third_party/llvm/install/` (and the tag cache at
  `build/scripts/llvm/.llvm_tag_cache`) to force re-resolution.
- Reference for all CMake flags of the LLVM build: run
  `cmake -N -L -S llvm -B /tmp/llvm-flags` in the fork, or read
  `llvm/docs/CMake.rst` there.
