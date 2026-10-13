# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [ ] Doc-comment collection in the parser (the SSG itself waits).
- [ ] `Box::into_inner` (and a recursive walk through `Box`) waits for a
  spent-handle representation: the destructor has to skip a block that
  was released, and the assignment that would say so (`self.cell = 0
  as *mut T`) does not appear in the emitted IR when a `read` and a
  `dealloc` precede it, so the destructor still sees a live handle.
  By-reference pattern bindings would cover the walk on their own.
- [ ] `map` has no constructor: its result type comes from a closure's
  return type, which inference cannot read before the closure is
  checked. A slice that checks a closure argument first, or that lets
  the adapter be spelled with a turbofish, gives it one.
- [ ] A discarded temporary that owns memory leaks: `_ := format("x")`
  allocates the string and the drop pass never ends it, because the
  value is not a binding the pass can schedule. The `fmt` expansion
  calls its collected tuple's glue directly for the same reason.
- [ ] Manifest lint: flag `0.x` dependency requirements. The grammar
  accepts them, but a pre-1.0 minor may break, so the advice belongs
  to a lint rather than to the parser (ADR-0057).
- [ ] Format-string lint: require the explicit `f` conversion
  (`{:.2f}` over `{:.2}`). The two spellings are the same piece, so
  only a project-level lint rule can insist on the explicit one.
- [ ] Link-time measurement: a benchmark reporting where a link goes —
  driver startup, the object write, the linker — and asserting nothing
  about wall time.
- [ ] The embedded linker beyond this host: startup discovery hard-codes
  Linux paths, only the ELF driver is embedded, and ADR-0049's M3 (the
  wasm lld driver and the portable full compiler) is unstarted.
- [ ] Diagnose a binary with no free `main` while checking. Today the
  program reaches the linker and reports `Linking failed`, which names
  the tool rather than the missing entry.
- [ ] `alcy check --file /dev/stdin` (any non-regular, zero-length input)
  reports success without reading the pipe; `compile --stdin` reads it.
- [ ] Container destructors do not end their elements. `Vec`'s and
  `Map`'s `drop` release the buffer and never run an element's
  destructor, so a one-element `Vec<String>` leaks that string's
  block (LeakSanitizer reports 4 bytes). The fix needs an
  element-ending walk, either in the container's own `drop` once the
  language can end a moved-out value, or as compiler glue over a
  container's elements.
- [ ] IR cache shared across profiles and machines. The lowered IR is
  one artifact per pointer width: a test lowers one source under two
  64-bit triples and compares the bytes (`pipeline/ir_emit_test.cc`),
  the release passes run after this boundary (ADR-0067), and the
  serialized form states the width in its header. The cache itself —
  a store keyed by package digest, dependency closure, format
  version, and pointer width, serving debug and release and every
  machine of a width — is the work. What it needs first is the
  summary-carrying artifact story (a library object is still opaque,
  `roadmap.md` item 1) and an answer for comp-folded `usize`
  constants, which currently bake the width into the immutables.
- [x] Inbounds GEPs and memcopy alignment. Lowering marks checked
  indexing, field projections, and aggregate construction inbounds,
  and the memcopy intrinsic carries the element alignment its two
  pointers share; serialization, the LLVM emitter, and the IR guide
  follow. The direct backend ignores both.
- [ ] Devirtualization of function values. A call through a value is
  an indirect call in both backends; LLVM sometimes folds it after
  inlining, and the direct backend never does. The adapters store
  their function in a struct field the caller fills, so the pass
  needs cross-function facts and lands once the pass stage grows a
  module pass.
- [ ] Bounds-check elision. Checking an index at every access costs a
  compare, a branch, and a panic block; a literal index the array's
  own length admits now skips the check entirely. What remains is the
  range analysis: `0..len` loops and repeated indices would remove
  the checks an earlier one dominates, and the `Switch` opcode stays
  unemitted for match chains.
- [ ] Index the side tables the proportionality work left behind. Most
  landed (ADR-0046); the tail is `expr_types` in both readers, the
  per-module closure, closure-fn, and indirect-call tables,
  `instantiate_fn`'s instance scan, `fn_args_for` and `closure_fn_index`,
  and the module statics. `Lowerer::expr_type` scans backward ("last
  entry wins") while `CompEvaluator::expr_type_in` scans forward ("first
  wins"), so the readers can disagree on a duplicate key, and
  `MAX_FN_ENTRIES = 8192` turns a package past it into an internal error
  rather than a diagnostic.
- [ ] Split `Lowerer`: comp evaluation, format expansion, and AST-to-IR
  lowering share one class with a large public surface.
- [ ] Borrow-pass scratch sized by the program, not the function: the
  arrays are allocated at package size and each function clears only the
  entries it names. The final pass's per-worker memory cost is why
  spreading it lost by measurement (ADR-0047), so the sizing waits for a
  reason to spread it.

### Language and library surface

- [ ] Qualified generic calls: `helper::id::<i32>(x)` is "Unresolved
  value 'id'" because the module-qualified path branches never reach the
  generic-function lookup, and an associated function's turbofish
  (`P::id::<i32>`) reads as the owner's type arguments. A single callee
  segment is the only spelling that registers a generic with its
  turbofish.
- [ ] Read-before-write through an operator: `v[i] = v[i] + 1` and
  `m[k] = m[k] + 1` are rejected as conflicting borrows; binding the read
  to a temporary first works. Two-phase borrows would let the idiomatic
  form compile.
- [ ] A returned error from `main() -> Result` aborts (SIGABRT, status 134)
  with "main returned Err". Decide whether the error variant is an exit
  status instead of a panic.
- [ ] Structural equality for tuples and arrays: `==` is builtin for
  scalars and addresses and spec-based for nominal types, so `(1, 2) ==
  (1, 2)` and `[1, 2] == [1, 2]` have no comparison. Decide the builtin
  element-wise rule or keep it explicit.
- [ ] Float ranges: `for x in 0.5..<2.5` fails with type errors inside
  `core/range.al`. `types.md` says float ranges have no iteration, so the
  iteration site wants one clear diagnostic instead of std internals.
- [ ] Explicit spec call with a temporary: `a.eq(&2)` fails lowering with
  "Construct 'borrowed temporary' is not supported in this lowering slice".
- [ ] `Option::is_some`/`is_none`/`is_ok`/`is_err` take the receiver by
  value, so the option cannot be consulted twice. `&Self` receivers
  would fix that.
- [ ] Standard dependency selection: `--deps alcy/std/collections` on top
  of the default suite errors "Dependency 'alcy/std/*' overlaps
  'collections'". Make the default suite and the glob rule visible in
  `--help` and the diagnostic.
- [ ] `Map` key ergonomics: no entry API (`get_or_insert` first, an
  `Entry` once the borrow rules allow), and a missing key's panic prints
  the `Option::unwrap` wording. Generic keys wait for a `Hash` spec and
  bounds; `keys()` traversal and `Vec` sorting moved to the roadmap's
  iteration and operator items.
- [ ] Trace report nesting: the one-pass scan picks the innermost open
  interval as parent, not the smallest containing one as its comment says,
  so overlapping events from two threads reshape the tree. The tree stays
  well formed; the rule and the code disagree.
- [ ] `demangle` recurses per type constructor with no depth limit; a
  hostile symbol can exhaust the stack. Latent while only tests call it.

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
- [x] Diagnostic code space: a letter per component, and each component
  counting its own ids from 1. The letters are `A` lexer, `B` parser,
  `C` analyzer, `D` lowering, `F` borrow, `G` ir, `H` pkg, `I` pipeline,
  `J` codegen_llvm, `K` codegen (native, reserved), with `E`/`N`/`W`
  held back for severity, so a code reads `error[EA001]`: severity,
  component, id. The reason is that the 1000-per-stage stride has no
  room for a second code-generating backend, that its sub-ranges are
  narrower than the stages that use them, and that a stage should not
  have to know the layout of the whole compiler to pick a number. The
  registry in `compiler/docs/diagnostics.md` stopped restating the
  numbers; the enums own them, and every id is spelled out.
  Designed in `docs/adr/0039-diagnostic-code-space.md`.
- [x] CI carries the gates: `lint`, the coverage ratchet, the sanitized
  exe run, `check.sh` itself, and the benchmark smoke are jobs in
  `ci.yaml`; `style.yaml` keeps what needs no compilation database. The
  smoke ran in all ten matrix entries, judging shape and not timing,
  which is ten answers to one question. Cost was not the obstacle for
  any of them - two documents called the coverage ratchet the slowest
  gate in `check.sh` and it is among the cheapest, which
  `tools/measure_gates.py` now reports rather than leaves to be believed.
  Designed in `docs/adr/0040-gates-live-beside-the-builds.md`. The
  `check.sh` job was later removed - it rebuilt what the matrix had just
  built and asked every gate a second time on one runner - and the union
  it stood for moved into `check.sh` itself, which now also runs the
  release-mode gates, the wasm acceptance, and the benchmark smoke; the
  ADR's amendments record the change.
- [x] `unsafe` as a gate on operations: `unsafe fn` and
  `unsafe { ... }` blocks, always lexical, with the block's value
  being its own. The precondition-carrying intrinsics are declared
  `unsafe` and the checker verifies the marker against its canonical
  set, the standard library wraps its uses, and the borrow checker's
  block handling was fixed so a move inside a block stays moved.
  Raw pointers followed: `*T` and `*mut T` are thin, Copy, and
  outside the region system, safe to create by cast (including
  `0 as *T`), and read, written, and offset behind the gate.
  `extern "C"` declarations followed: bodyless functions whose names
  are the linker's symbols, called behind the gate, over the ABI of
  scalars, raw pointers, and `()`.
  A freestanding link mode followed: `freestanding = true` in the
  toolchain file, the compiler's `_start`, a per-declaration runtime
  reaching the kernel through raw syscalls, and an `mmap`-backed
  allocator, so `print` and the heap containers run with no crt, no
  loader, and no libc.
  Designed in `docs/adr/0050-unsafe-is-a-gate-on-operations.md`,
  `docs/adr/0051-extern-c-for-a-minimal-abi.md`, and
  `docs/adr/0052-freestanding-is-a-link-mode.md`.
- [x] alcy IR format: define, serialize, deserialize; ahead of
  `--emit=ir` and any cache. The text form is write-only by decision
  (`docs/adr/0056-the-ir-has-a-text-and-a-binary-form.md`) and the
  binary form round-trips byte for byte; the cache is still to come.
