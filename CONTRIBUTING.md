# Contributing

## Prerequisites

- A C++20 toolchain: Clang, LLD, and libc++ (see [docs/build.md](docs/build.md)).
- GN and Ninja.
- Python via `uv` (`uv sync` sets up the environment; `uv run` prefixes commands).
- Alternatively, Nix provides the whole toolchain: `nix develop`.

## Workflow

Build, test, and check from the repository root:

```bash
uv run ./build/scripts/build.py --target=default --mode=debug
uv run ./build/scripts/run.py --target=tests --mode=debug
uv run ./build/scripts/lint.py
uv run ./build/scripts/format.py --dry-run
```

Use `./build/scripts/format.py` (without `--dry-run`) to apply formatting.
CI runs the same steps for debug and release on Linux, macOS, and Windows,
plus a spell check (`spelling.yaml`), so please make sure all of them pass
before requesting a review.

## Conventions

- Follow [ARCHITECTURE.md](ARCHITECTURE.md) for module responsibilities.
  New compiler code belongs in the module that owns its stage; shared code
  goes in `core`, `base`, or `third_party` wrappers.
- C++20, no exceptions, no RTTI in code interacting with LLVM APIs.
- Style is enforced, not debated: `.clang-format`, `.clang-tidy`,
  `CPPLINT.cfg`, `stylua.toml`, and `typos.toml` are authoritative.
  New source files must carry the license header (checked by `format.py`).
- One decision, one record: significant technical decisions get an ADR in
  `docs/adr/` (copy `docs/adr/0000-template.md`). Small, obvious changes do
  not need one.

## Commit scope

Keep changes focused; avoid mixing refactors with behavior changes.
The project is pre-MVP, so APIs and the IR are still allowed to change,
but please call out breaking changes in the commit message.
