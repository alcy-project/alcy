# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [ ] Doc-comment collection in the parser (the SSG itself waits).
- [ ] Manifest lint: flag `0.x` dependency requirements. The grammar
  accepts them, but a pre-1.0 minor may break, so the advice belongs
  to a lint rather than to the parser (ADR-0057).
- [ ] Link-time measurement: a benchmark reporting where a link goes —
  driver startup, the object write, the linker — and asserting nothing
  about wall time.
- [ ] Embedded lld, and the system driver with it. The driver picks
  crt files, system libraries, and search paths per platform, so this
  follows the item above and stands as its own project.
- [ ] Diagnose a binary with no free `main` while checking. Today the
  program reaches the linker and reports `Linking failed`, which names
  the tool rather than the missing entry.
- [ ] Drop elaboration as a pass over IR. Lowering now ends each live
  path's values in that path's own epilogue (match arms, if arms, and a
  moved field is skipped where it left), which fixed the double drops and
  the `or`/`unwrap_or` leaks. What remains is the borrow pass: a move on
  one path retires the value on every path, so `if c { take(r) }` and
  `Option::filter` over a non-Copy `T` still report use-after-move where
  the owning path would end the value. A pass that consumes the `Move`
  markers and solves ownership per CFG path makes `break`, `continue`,
  `?`, and these joins one rule.
- [ ] Index the analyzer's and lowerer's side tables by their keys
  (expression types, type origins, generic instances, function
  specializations). Each lookup is a linear scan today, so checking and
  lowering grow quadratically with the package.
- [ ] Split `Lowerer`: comp evaluation, format expansion, and AST-to-IR
  lowering share one class with a large public surface.
- [ ] Size borrow-pass scratch by the function, not the package: each
  function's reset clears arrays as large as the whole program today.

### Language and library surface

- [ ] Container iteration: an `into_iter`/cursor for `Vec`, `Map`, `Set`,
  arrays, and slices, so `for` can walk the standard containers. Iterator
  adapters (`map`, `filter`, `collect`) are roadmap work; until the cursors
  land, algorithms index-loop instead.
- [ ] Qualified generic calls: `helper::id::<i32>(x)` is "Unresolved value
  'id'". Only a single-segment callee registers a generic function with its
  turbofish; a module-qualified one never reaches `lookup_generic_fn`.
- [ ] Spec methods on standard types from user code: `mut r := 0..<5;
  r.next()` is rejected with "No method 'next'" although the same spec
  implemented in the caller's package works, and a generic
  `fn sum<I>(it: I)` over a standard iterator cannot be written. Find why
  cross-package spec implementations do not resolve for an explicit call
  (the `for` desugar dispatches internally and hides it).
- [ ] The implementing side of sealed operator specs: a user package cannot
  write `impl PartialEq for P` or `impl Index for G`; the diagnostic says
  the `[spec] implement` manifest key is not accepted yet (ADR-0053,
  `deferred.md`). Ship the key.
- [ ] Indexing a container field through `self`: `self.data[0]` in a method
  is rejected as "does not implement the Index spec" while
  `self.data.as_slice()[0]` works. A field's storage-copy type is not
  followed to its origin in operator lookup.
- [ ] Read-before-write through an operator: `v[i] = v[i] + 1` and
  `m[k] = m[k] + 1` are rejected as conflicting borrows; binding the read
  to a temporary first works. Two-phase borrows would let the idiomatic
  form compile.
- [ ] Recursive data structures: a recursive enum or struct needs raw
  pointers and manual `alloc` today; there is no owning indirection
  (`Box`-equivalent). Design one with the owning environment follow-up
  (ADR-0044).
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
- [ ] Float formatting: `fmt.md` defers it (integers, `bool`, and `str`
  are the convertible types). Implement when the formatter grows precision
  handling.
- [ ] Discards of values with destructors: `_ := expr` is an error while a
  bare expression statement only warns, and neither ends the value. Decide
  one rule for both spellings.
- [ ] Explicit spec call with a temporary: `a.eq(&2)` fails lowering with
  "Construct 'borrowed temporary' is not supported in this lowering slice".
- [ ] `Option::is_some`/`is_none`/`is_ok`/`is_err` take the receiver by
  value, so the option cannot be consulted twice and a payload ends only
  through the enum walk. `&Self` receivers would fix both.
- [ ] An enum's payload has no drop glue: `match` on a wildcard arm (as
  `is_some` writes) discards a `String` payload without ending it, and
  the same holds for arrays. Elaborating payload drops is part of the
  drop pass above; until then the warning names the binding.
- [ ] Standard dependency selection: `--deps alcy/std/collections` on top
  of the default suite errors "Dependency 'alcy/std/*' overlaps
  'collections'". Make the default suite and the glob rule visible in
  `--help` and the diagnostic.
- [ ] `Map` key ergonomics: keys are `str`-only and `Set` holds strings
  only; `m["missing"] = v` panics with the `Option::unwrap` wording; there
  is no entry API, no `keys()` traversal, and no sort on `Vec`. The
  containers package is the place for these.
- [ ] Trace report nesting: the one-pass scan picks the innermost open
  interval as parent, not the smallest containing one as its comment says,
  so overlapping events from two threads reshape the tree. The tree stays
  well formed; the rule and the code disagree.
- [ ] `demangle` recurses per type constructor with no depth limit; a
  hostile symbol can exhaust the stack. Latent while only tests call it.
- [ ] Stale module comments: `lib/std/collections/{vec,map,set}.al` still
  name `alcy/std/alloc`.

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
  Designed in `docs/adr/0040-gates-live-beside-the-builds.md`.
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
