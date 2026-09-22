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

`default` builds the compiler group only; test and benchmark executables
are not referenced by any group, so they build solely under `all` (or when
named explicitly, e.g. `--target=tests`).
For a complete list of supported flags and options, pass `--help` to any script.

Build output goes to `out/<build-subdir>/` (`out/build/` by default).

## How the LLVM dependency works

The compiler links against prebuilt LLVM binaries built from a private LLVM
fork (`llvm-alcy-fork`). The required version and release tag are configured in
`config.toml` (`llvm_fork_tag`), which serves as the single source of truth (SSOT).

At GN time, the `//third_party/llvm:setup_llvm` action prepares a ready-to-link
installation under `out/<build-subdir>/third_party/llvm/install/<debug|release>/`
in this order:

1. **Reuse**: if the installation directory exists and its recorded tag matches
    `llvm_fork_tag` in `config.toml`, nothing is done.
2. **Download**: otherwise, a prebuilt archive corresponding to `llvm_fork_tag`
    and the target triple is fetched from the fork's GitHub Releases
   (`llvm-<debug|release>-<triple>.tar.zst`, `.zip` on Windows) and extracted.

To update the LLVM dependency, change `llvm_fork_tag` in `config.toml`. GN and
Ninja will automatically detect the change and trigger `setup_llvm.py` to fetch
the updated prebuilt release.

## Platform notes

- **Linux/macOS**: libc++ is used (`-stdlib=libc++`); the `LIBCXX_*`
  environment variables point GN at its headers and libraries.
- **Windows**: the MSVC STL is used (no libc++). Both the compiler and the
  prebuilt LLVM libraries use the static C runtime (`/MT`, `/MTd`); mixing
  runtimes fails the link with
  `lld-link: error: /failifmismatch: mismatch detected for 'RuntimeLibrary'`.
  If you see this error, the LLVM installation was built with a different
  CRT - re-run the setup after removing the stale install directory.
- **WebAssembly**: built with Emscripten (`emcc`/`em++` on `PATH`) targeting
  `wasm32-unknown-emscripten`, using Emscripten's bundled libc++ (the
  `libcxx` GN config is excluded). Always use a dedicated output directory
  (e.g. `--build-subdir=build_wasm --target-os=emscripten`); reusing a native
  output directory leaves stale artifacts behind. Test binaries run under
  `bun` (falls back to `node` if `bun` not found. `run.py` handles this automatically).

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
builds additionally pass `-Xlinker /NODEFAULTLIB:libcmt.lib -Xlinker /DEFAULTLIB:libcmtd.lib` (same file, `linker` config). 
Do not remove those flags without re-checking the driver behavior.

To verify which CRT an artifact uses, inspect its directives, e.g.
`llvm-readobj --coff-directives <lib>.lib | grep RuntimeLibrary` should show
`MT_StaticRelease` / `MTd_StaticDebug` for alcy objects and LLVM libraries
alike.

## Troubleshooting

- `gn gen` failures: make sure `gn` and `ninja` are on `PATH`
(`nix develop`, or install them via your package manager).
- Stale LLVM install: delete
`out/<build-subdir>/third_party/llvm/install/`  to force re-resolution.

