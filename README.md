# alcy programming language

[![CI](https://github.com/alcy-project/alcy/actions/workflows/ci.yaml/badge.svg)](https://github.com/alcy-project/alcy/actions/workflows/ci.yaml)

alcy is an experimental systems language. It is statically typed and
move-only, it is memory safe without lifetime annotations, and it spans
bare metal to high-level software with one compiler.

```alcy
fn main() {
  println("hello, world")
}
```

[Run this program in the browser](https://alcy-project.github.io/alcy/playground/).

A function that returns a status declares the type, as in
`fn main() -> i32`. Ownership is not written at the call site: region
inference over function summaries works out what a signature would
otherwise have to spell out. [principles.md](docs/principles.md) argues
for the design, and the
[tour of the language](docs/guide/en/03-language-tour.md) covers what the
compiler accepts today, with every example on that page compiled when the
site is built.

The [alcy site](https://alcy-project.github.io/alcy/) carries that guide,
the language reference, and a browser playground: edit and run alcy in a
tab, with no install.

## Status

Pre-1.0. The language, its standard library, and parts of the compiler
pipeline are still being designed. Nothing here promises stability, and
there is no backward compatibility guarantee before 1.0: a program that
compiles today may need an edit tomorrow.

- [docs/spec/](docs/spec/) - the specification of the language as it is
  meant to be. `deferred.md` lists what is not implemented yet.
- [src/docs/architecture.md](src/docs/architecture.md) - how the
  compiler is put together.
- [samples/](samples/) - small programs kept because they are worth
  reading, `fizzbuzz` and `wordfreq` among them.

## What alcy is

alcy compiles to stand-alone binaries (LLVM backend) or to WebAssembly
(`--backend=direct-wasm`; alcy's own WASI emitter, with no LLVM and no
linker behind it). There is no interpreter and no runtime to ship: a
compiled program runs on its own.

It builds and tests on Linux (x86_64 and arm64), macOS (aarch64 and
x86_64), and Windows (x64 and arm64), which is what CI runs;
`wasm32-unknown-emscripten` is also a target.

A freestanding build - `.alcy/toolchain.toml` with `freestanding = true` -
links without a C runtime and without libc: the compiler emits `_start`,
and the pieces that need the kernel talk to it with raw syscalls
(x86-64, aarch64, riscv64), so a freestanding program carries only what
it uses. See [the specification](docs/spec/ffi.md#freestanding-bootstrap).

`comp` evaluates at build time and is carried by nothing at runtime.
The language is pre-1.0; see [docs/spec/](docs/spec/) for what a program
is written against, and [src/docs/architecture.md](src/docs/architecture.md)
for how the compiler is put together.

## Building

Requires GN, Ninja, Clang, LLD, and libc++ on Linux/macOS, or the MSVC
STL on Windows (see [src/docs/build.md](src/docs/build.md) for details):

```bash
# Using Nix (Linux / macOS):
nix develop
uv run ./tools/build.py

# Without Nix (after installing the toolchain above):
uv run ./tools/build.py
```

Run the tests with:

```bash
uv run ./tools/run.py --target=tests
```

## Installing

Download the archive for your platform from
[Releases](https://github.com/alcy-project/alcy/releases) and verify the
`.sha256` beside it before extracting (`sha256sum -c` on Linux,
`shasum -a 256 -c` on macOS). The archive carries the `alcy` binary.

Linking what you build: on Linux, macOS, and Windows the binary links
in-process with the lld the compiler carries. Linux hosted links the
system C runtime by default; a freestanding build links without one.
Windows links `kernel32.dll` and nothing else; macOS links
the SDK's `crt1.o` and `libSystem`, the SDK discovered from `SDKROOT`,
`DEVELOPER_DIR`, or the usual Command Line Tools or Xcode SDK
locations (the Windows SDK is discovered the same way, from
`WindowsSdkDir` or the kits' `Lib` directories). A host with no SDK
falls back to a `clang` from PATH — and a freestanding build links
without a C library anywhere (macOS aside, where `freestanding = true`
is refused: libSystem is the only sanctioned route there). A `linker`
in `.alcy/toolchain.toml` or `--linker` replaces the default.

In a workflow, `uses: alcy-project/alcy@vX.Y.Z` does the same thing:
download, verify, and put `alcy` on PATH.

To install a source build instead, copy `out/build_release/alcy` to a
directory on your PATH, such as `~/.local/bin`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0 with LLVM Exceptions. See [LICENSE](LICENSE).
