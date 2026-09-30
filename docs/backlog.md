# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [ ] Noun-form audit: modules, structs, and classes read as nouns
  (stdlib and compiler alike).
- [ ] `unsafe` design as an ADR; implementation waits for the package
  suite.
- [ ] Doc-comment collection in the parser (the SSG itself waits).
- [ ] alcy IR text format: define, serialize, deserialize; ahead of
  `--emit=ir` and any cache.
- [ ] Lib packages: the suite's other half next to `[[bin]]`.
- [ ] Link-time measurement: a benchmark reporting where a link goes —
  driver startup, the runtime's own compilation, the linker — and
  asserting nothing about wall time.
- [ ] Program runtime emitted in process: the `alcy_*` functions are
  defined in the module our own codegen already builds, so no clang
  compiles `alcy_runtime.c` on every build. The pinned LLVM ships no
  clang, and a host-made `.bc` would couple every build to its version,
  so the functions are built as IR instead.
- [ ] Embedded lld, and the system driver with it. The driver picks
  crt files, system libraries, and search paths per platform, so this
  follows the two items above and stands as its own project.

## Shipped

- [x] Verb split: `compile` takes a file, `build` a package, `check`
  neither emits. The three verbs each sniffed the target's extension, so
  the sniff lived in three places and `build` carried options for two
  jobs. Designed in `docs/adr/0018`.
- [x] Per-goal configuration under `.alcy/`: `alcy.toml` declares what
  a package is, so build choices moved out of it rather than growing
  beside it. Designed in `docs/adr/0019`.
- [x] Version control option for `new`/`init`: scaffold the ignore
  file for the chosen VCS, never initialize a repository.
- [x] Command result envelope: one output path rendering as text or
  `--json`, with statistics in the text line and the time trace
  embedded alongside it. Designed in `docs/adr/0020`.
- [x] Result lines as a column: counts carry their nouns, sizes scale
  past a kibibyte, and the verb sits at a fixed width so a session's
  results read as a table. A run announces itself before the program
  starts rather than reporting below its output.
- [x] Optimized builds: wire the optimization level through to LLVM
  behind the existing `--release`. The flag reached the object file and
  not the textual IR, because the pass pipeline ran inside the emitter
  rather than on the module; every backend now sees one module.
- [x] Benchmark harness: an engine for the compiler's phases beside the
  process runner, with a local reproducibility check and a CI smoke
  check. Designed in `docs/adr/0021`. Nine of the seventeen phases have
  cases; `load`, `prelude`, and `link` have none on purpose, since each
  is a read, a staging of the embedded suite, or a subprocess, and a
  number for those belongs to the process runner.
- [x] `grammar.ebnf` maintenance: every grammar change diffs the file
  in the same commit.
- [x] `build/scripts` → `tools/` rename, in one shot.
- [x] Samples: `fizzbuzz`, `fibonacci`, and `primes`, run through the
  `exe` harness by `--cases-root` so an example that stops compiling
  fails the build.
- [x] `--emit=llvm-bc`: the module as LLVM's bitcode — the same content
  `--emit=llvm-ir` prints, in the form another LLVM tool reads without
  parsing text, and the second mode that needs no target.
- [x] Link arguments: `--link-args` on `build`, `run`, and `compile`,
  plus `link-args` in `.alcy/toolchain.toml`. They are the driver's own
  arguments, so a library or `-fuse-ld=lld` reaches it unquoted, and a
  flag replaces the file's list the way `--linker` replaces the file's
  driver. Before it, no external library could be linked at all.
