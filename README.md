# alcy programming language

alcy is an experimental systems language. It is statically typed and
move-only, it is memory safe without lifetime annotations, and it spans
bare metal to high-level software with one compiler.

```alcy
fn main() {
  println("hello, world")
}
```

A function that returns a status declares the type, as in
`fn main() -> i32`. Ownership is not written at the call site: region
inference over function summaries works out what a signature would
otherwise have to spell out. [principles.md](docs/principles.md) argues
for the design, and the [tour of the
language](docs/guide/03-language-tour.md) covers what the compiler
accepts today, with every example on that page compiled when the site
is built.

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

## Build & Install

Requires GN, Ninja, Clang, LLD, and libc++ (see [src/docs/build.md](src/docs/build.md)
for details).

```bash
# Using Nix (Linux / macOS):
nix develop
uv run ./tools/build.py

# Without Nix (after installing the toolchain above):
uv run ./tools/build.py

# Build and install (requires nix)
./tools/install.sh
```

Run the tests with:

```bash
uv run ./tools/run.py --target=tests
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0 with LLVM Exceptions. See [LICENSE](LICENSE).
