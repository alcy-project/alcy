# alcy programming language

alcy is an experimental statically-typed programming language.
The project is in early development and not yet usable.

## Status

Pre-MVP. The language specification, standard library, and most of the
compiler pipeline are still being designed and implemented.

- [PRINCIPLES.md](PRINCIPLES.md) — where the language is going, and the
  properties that decide design questions.
- [ARCHITECTURE.md](ARCHITECTURE.md) — the planned compiler design.

## Build & Install

Requires GN, Ninja, Clang, LLD, and libc++ (see [docs/build.md](docs/build.md)
for details).

```bash
# Using Nix (Linux / macOS):
nix develop
uv run ./build/scripts/build.py

# Without Nix (after installing the toolchain above):
uv run ./build/scripts/build.py

# Build and install (requires nix)
./build/scripts/install.sh
```

Run the tests with:

```bash
uv run ./build/scripts/run.py --target=tests
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0 with LLVM Exceptions. See [LICENSE](LICENSE).
