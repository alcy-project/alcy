# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [ ] Diagnostic code space: a letter per component, and each component
  counting its own ids from 1. `docs/adr/0037-diagnostic-code-space.md` is
  the decision; the letters are `A` lexer, `B` parser, `C` analyzer, `D`
  lowering, `F` borrow, `G` ir, `H` pkg, `I` pipeline, `J` codegen_llvm,
  `K` codegen (native, reserved), with `E`/`N`/`W` held back for
  severity. A code reads `error[EA001]`: severity, component, id. The
  reason is that the 1000-per-stage stride has no room for a second
  code-generating backend and its sub-ranges are narrower than the
  stages that use them, and that a stage should not have to know the
  layout of the whole compiler to pick a number. The registry in
  `compiler/docs/diagnostics.md` stops restating the numbers; the enums
  own them.
- [ ] CI: `lint`, the coverage ratchet, and the sanitized exe run as jobs
  in `ci.yaml`, plus `check.sh` itself as a Linux job so the gate a
  contributor runs is the gate CI runs. `style.yaml` keeps what needs no
  compilation database. The benchmark smoke moves out of the ten matrix
  entries into one job, since it judges nothing about timing and costs
  156s in each. `tools/measure_gates.py` reports the numbers, so the
  runtime claims in `docs/adr/0017-verification-strategy.md` and
  `CONTRIBUTING.md` can be measured rather than believed; both currently
  call the coverage ratchet the slowest gate and it is the cheapest.
- [ ] `unsafe` design as an ADR; implementation waits for the package
  suite.
- [ ] Doc-comment collection in the parser (the SSG itself waits).
- [ ] alcy IR text format: define, serialize, deserialize; ahead of
  `--emit=ir` and any cache.
- [ ] Lib packages: the suite's other half next to `[[bin]]`.
- [ ] Link-time measurement: a benchmark reporting where a link goes —
  driver startup, the object write, the linker — and asserting nothing
  about wall time.
- [ ] Embedded lld, and the system driver with it. The driver picks
  crt files, system libraries, and search paths per platform, so this
  follows the item above and stands as its own project.

## Shipped

- [x] Noun-form audit: modules, structs, and classes read as nouns
  (stdlib and compiler alike). The rule is that the last word of a name
  is a noun, recorded in `CONTRIBUTING.md`; the audit found `lower`
  (now `lowering`), `Desugar` (now `Desugarer`), `FmtParse` (now
  `FmtTemplate`), and `VerifyError` (now `VerificationError`, which also
  retired a name two modules shared), plus three ways one concept was
  spelled. Stdlib needed nothing. `debug` keeps its name as the one
  recorded exception.
- [x] Verb split: `compile` takes a file, `build` a package, `check`
  neither emits. The three verbs each sniffed the target's extension, so
  the sniff lived in three places and `build` carried options for two
  jobs. Designed in `docs/adr/0018-compile-build-check-split.md`.
- [x] Per-goal configuration under `.alcy/`: `alcy.toml` declares what
  a package is, so build choices moved out of it rather than growing
  beside it. Designed in `docs/adr/0019-goal-config-in-dot-alcy.md`.
- [x] Version control option for `new`/`init`: scaffold the ignore
  file for the chosen VCS, never initialize a repository.
- [x] Command result envelope: one output path rendering as text or
  `--json`, with statistics in the text line and the time trace
  embedded alongside it. Designed in `docs/adr/0020-command-result-envelope.md`.
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
  check. Designed in `docs/adr/0021-benchmark-measurement.md`. Nine of the seventeen phases have
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
- [x] Runtime emitted in process: the `alcy_*` functions are defined in
  the module our own codegen already builds, so no compiler is spawned
  for them on every build, the link line loses an input, and the
  runtime is optimized with the program. A hello-world's link phase
  went 60.1 ms → 29.2 ms and its total 114 ms → 85.2 ms. The pinned
  LLVM ships no clang and a host-made `.bc` would couple every build to
  its version, so the functions are built as IR rather than linked in.
  Designed in `docs/adr/0023-program-runtime-in-process.md`.
