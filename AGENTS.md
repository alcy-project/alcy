# Context

`alcy` is an AOT C++20 compiler (no interpreter/runtime).

- **Host:** Linux (`x86_64`), macOS (`aarch64`), Windows (`x64`).
- **Targets:** Native binaries (LLVM), WASI (`--backend=direct-wasm`), `wasm32-unknown-emscripten`. Freestanding builds need no `libc` (see `README.md`, "What alcy is", and `docs/spec/ffi.md`).

# Docs

- **Always read first:** `CONTRIBUTING.md` (`build.py`, `lint.py`, formatting, commits).
- **Read as needed:** `src/docs/architecture.md` (pipeline, IR, modules), `docs/spec/` (language rules; the record a change is reviewed against — a change that revisits a rule amends it), `docs/backlog.md`, `docs/roadmap.md`.

# C++20 & Style Rules

- **Idioms & Memory:** Use C++20 (`std::span`, `std::optional`, concepts, designated initializers). Enforce value semantics or smart pointers (`unique_ptr`/`shared_ptr`); no raw `new`/`delete`. Default to `const` and pure functions.
- **No Exceptions / No RTTI:** Built with `-fno-exceptions -fno-rtti`. Never use `try`/`catch`/`throw`, `dynamic_cast`, or RTTI. Report errors via `std::optional`, `Result`, `AutoTaggedUnion`, or diagnostic handlers. Assert internal invariants with `DCHECK()` or `src/base/` & `src/debug/` logging.
- **Comments:** Prefer self-documenting types/names. Keep comments concise and maintenance-focused. Do not justify, narrate, or restate obvious code; never leak session/prompt context (BAD: `// No std::filesystem, we don't use it here`, `// Per user request`, `// Omitted X`).

# Code Generation & Workflow

- **Boundaries & Build:** Obey `src/*` module boundaries and the linear pipeline in `src/docs/architecture.md`. Add license headers and `BUILD.gn` entries to all new files. Use English for all code, comments, docs, and commits.
- **Logs:** Redirect verbose tool output to `/tmp/alcy-<branch>-<check>.log` (e.g., `LOG="/tmp/alcy-<branch>-lint.log"; uv run tools/lint.py > "$LOG" 2>&1 || tail -n 50 "$LOG"`).
- **Search & Edit:** Scope searches with `rg "pattern" src/ -l` and `| head -n 30`. Check `git diff --stat` before full diffs. Prefer `ast-grep` over manual edits for repetitive structural changes.
