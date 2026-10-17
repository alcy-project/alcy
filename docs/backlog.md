# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [x] Doc-comment collection in the parser: `skip_insignificant` keeps
  the span of each `DocComment` it walks past, the item loop hands an
  item the ones written above it, and `ItemNode::docs` carries them as
  spans into the same bytes. The lexer already tokenized them, so the
  site generator is the only reader left to write.
- [x] The direct-wasm backend typed a field or element address by the
  register beside it, so a `GetElementPtr` into an `f64` declared an
  `F64` local and handed it an address: any program that formats a
  float emitted an invalid module. Addresses are words whatever they
  point at (`shape_of_definition`), `e2e/wasm/float-format` covers the
  spelling, and the guide's `{:.2f}` example is a runnable fence again.
- [x] Consuming `for x in a` over an array failed on direct-wasm: the
  backend's pointer lists named `Ptr`, `Ref`, `MutRef`, and `Function`
  but not the two raw pointers, so the `*T` a cursor keeps in
  `core/slice.al` was "not supported" (EK001). One predicate
  (`is_thin_pointer`) names every tag that rides as an address word
  now, and `e2e/wasm/for-array` walks an array and a `Vec` by value;
  the guide keeps `.iter()` because a by-reference walk is what the
  example wants, not because the other was refused.
- [x] Moving a non-`Copy` element out of an owned array is refused now
  (`lower_moved` sees an array element under the value and answers
  `MoveOutOfElement`), where it used to load a copy that the array's
  glue ended a second time - a double free in safe code. The refusal
  covers a field of an element too (`a[0].name`), and an intrinsic's
  by-value argument takes the same road.
- [ ] Moving an element out of an owned array, which the refusal above
  stands in for: the drop walk's paths name fields and tuple slots, so
  the pass needs an element step that ends the elements the move left
  behind. A value moved out of a temporary array (`f().a[0]`) is the
  same shape one level out.
- [x] Matching over a borrowed value whose type carries a non-`Copy`
  payload reads it in place: the discriminant is read where it lies, a
  pattern's bindings are shared references, and moving a payload out
  stays refused (`err-borrowed-move-out`). A `Copy` payload binds as
  `&T` too, so one rule covers every arm.
- [x] `map` has a constructor: a call reads the closure argument's
  recorded function type, so the element type infers unspelled -
  `v.map((x: &T) -> U)` (`ok-vec-map`, `exe-vec-map`) - and the same
  reading fills a free function's return slot (`ok-closure-infer`).
- [ ] An associated function's call infers nothing: the member is
  resolved before its call's arguments are read, so
  `Holder::<i32>::map_to(...)` still spells what a method call now
  infers; a spelled turbofish also counts the synthetic `fn()`
  parameters a declaration carries.
- [ ] Negative comp-arg cases for the loosened predicates: a runtime
  value inside a match arm or a block (`double(match runtime_val …)`,
  `double({ y := runtime; y })`) must still be refused, the guard
  check a match now performs wants its own pin, and a generic caller's
  other-instantiation variant record wants one case proving the
  predicate's miss stays a compile error.
- [ ] The compiler's stack on a small-stack host: compiling to an
  object needs most of 64 KiB even natively, so the wasm test binary
  runs with a 1 MiB stack while programs it builds keep the default.
  Any standard library growth showed up as a gate failure rather than
  a number; find the deep path, or budget the stack, before the
  headroom runs out the same way.
- [ ] References into the type tables held across checking: a struct
  literal's check kept a `StructType&` while a field value's check
  could intern a type and move the table (a heap-use-after-free the
  new std module's layout shift exposed; the struct case copies now).
  The analyzer keeps the same shape in its field, tuple, and enum
  readers, and each wants the same audit.
- [x] A discarded temporary that owns memory ends where it stands:
  `_ := format("x")` and a bare `format("x")` are both ended by the
  discard path's `record_drop_in_place`, which the sanitized runs of
  `exe-discard-end` confirm. The `fmt` expansion still calls its
  collected tuple's glue directly, because the tuple is a scratch slot
  rather than a binding the pass could schedule.
- [ ] A destructible local whose end site sits across a loop keeps its
  heap: the ownership dataflow seeds every block with `Moved`, so a
  loop's back edge joins `Maybe` at its header and every later site
  reads maybe - and the guarded drop never runs, because the guard's
  block and the flag it tests do not reach the emitted function (the
  dataflow reports the flag valid and the glue resolved while the
  module carries no `i1` alloca and no drop call). `s := format("ab{}",
  0)` followed by `g := [s] (x: i32) -> x + s.len() as i32` leaks the
  buffer, and the same witness through a free function's `fn()`
  parameter leaks too (`apply([s] ...)`); `exe-method-infer`'s witness
  holds only Copy values because of it. The seeding is the suspected
  cause - a bottom state the join can skip is the direction - and the
  guard's emission needs tracing to where the block is lost.
- [x] Manifest lint: a pre-1.0 version range warns (WH007), since a
  minor may break under 1.0. The rule reads the parsed requirement -
  one `Equal` bound is a pin and stays silent - so `0.5.x`,
  `>=0.5.0, <1.0.0`, and a stray leading space all warn.
- [x] The requirement message named forms the grammar refuses: `1.2`
  and `1` are not terms - `version_req_test` pins what parses, a
  three-component version or one whose last component is `x` - while
  the message read as if they were. The message names the forms that
  parse (`1.2.3`, `1.2.x`, `1.x`) and `err-dep-version-form` pins it.
- [x] Format-string lint: the explicit `f` conversion (`{:.2f}` over
  `{:.2}`). The analyzer warns (WC045) and names the spelling to
  write; both spellings stay one piece, so the parser is unchanged.
- [x] The benchmark fixtures' own commands were outside the gate: smoke
  checked that every case was present and that a generated source still
  compiled, but not that a case's command succeeded, so three fixtures
  rotted silently when the private-access check landed
  (`build/executable`, `check/package`, and `check/wide-package`).
  Smoke runs each process case once with its `expected_exit` now, and
  its first run found the second and third.
- [x] Link-time measurement: the embedded linker's one-time startup
  probe and the `lldMain` call are their own trace rows
  (`lld-startup`, `lld-link`) beside the object write `emit-object`,
  so `alcy build --time-trace` reports where a link goes. The suite's
  `build/executable` case measures the wall time and asserts nothing
  about it, which is what the benchmarks README says.
- [ ] The embedded linker beyond this host: startup discovery hard-codes
  Linux paths, only the ELF driver is embedded, and ADR-0049's M3 (the
  wasm lld driver and the portable full compiler) is unstarted.
- [x] A binary with no free `main` is diagnosed before the linker
  sees it: `check_entry_point` runs per resolved target, so `check`
  and a `build` that would link both name the target (EI024). An
  associated `main` is not an entry point, and a library target needs
  none.
- [x] A non-regular input is refused rather than read as empty: a pipe
  or a device reports no size and cannot be mapped, so `check --file`
  on one would check a program out as nothing. `SourceManager::load`
  answers `NotRegular`, and `check` and `compile` name the path; the
  POSIX-guarded unit test uses `/dev/null`. A pipe still reaches the
  compiler through `compile --stdin`.
- [x] Container destructors end their elements. `Vec::drop` clears
  through `drop_in_place` once per live element, and `Map::drop` moves
  each live pair out of its slot into a binding the drop pass ends.
  The sanitized runs of `exe-vec-drop` and `exe-map-drop-element`
  report nothing left allocated; both walked a container of values
  whose destructors own a block.
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

- [ ] The formatter's fixed-point digits are exact only to about
  sixteen places: `{:.Nf}` takes each digit by multiplying the fraction
  by ten in `f64`, so the error compounds and the seventeenth place is
  wrong (`{:.17f}` of `0.1` prints `0.10000000000000000` where half-up
  rounding of the exact value is `0.10000000000000001`; `1/3` prints
  `0.33333333333333330` against `0.33333333333333331`). An exact
  conversion over the mantissa, a scaled long division, is the fix; the
  spec's cap of 17 places stands.
- [ ] The direct-wasm stack budget: a frame past the reserve (the
  shadow stack's base) panics with "stack overflow", which is a
  per-target limit native does not share and no spec states; a
  variable-length alloca still walks past it unchecked, and
  `too-many-locals` provokes the local-cap report through release
  register counts, so it wants rechecking when regalloc-adjacent work
  lands.
- [ ] Owning function types (ADR-0075) beyond parameters and returns:
  a method's `fn(...)` parameter infers from its arguments, a turbofish
  spells only the parameters a declaration writes, and the return
  position takes its witness from the body's first `ret` - free
  functions and methods alike (`ok-own-fn-return`,
  `exe-own-fn-return`, `err-own-fn-return-mismatch`,
  `err-own-fn-return-missing`, `ok-own-fn-turbofish`). What remains: a
  spelling nested inside another type (`-> Option<fn(i32) -> i32>`, a
  struct field) still refuses (`err-own-func-type`); `fn()`-typed
  bindings are not yet move-only (reassignment must be refused); and
  named storage (`struct H { f: fn(...) }`) needs generics threaded by
  hand.
- [x] Two-phase borrows: a call's exclusive receiver is a reservation
  taken before the arguments evaluate, and the call itself activates
  it, so `v.push(v.len())`, `v.push(v[0])`, and
  `v.insert(v.len(), x)` resolve. A read that is still live at the
  activation - a view handed in as an argument, a closure that captured
  the place - still reports (`ok-borrow-two-phase`,
  `exe-borrow-two-phase`, `err-borrow-two-phase-arg`,
  `err-borrow-two-phase-escape`).
- [x] A method that takes its receiver by value moves a field receiver
  out of its owner: the call marks the field's place moved, so
  `s.b.unwrap()` with `b: Option<Vec<i32>>` retires the payload once
  (`ok-field-receiver`, `exe-field-receiver`).
- [x] A matched non-Copy field is moved out of its owner: the match
  marks the field's place moved, so `match s.b` with
  `b: Option<Vec<i32>>` retires the payload once
  (`ok-match-field-move`, `exe-match-field-move`).
- [x] A field hands back the type it declared: the reader returned the
  stored copy, and an enum field lost its variants with it — a match
  on `s.b` reported "Variant is not a member of the matched type" and
  `s.b.unwrap()` aborted, while a local of the same type was fine
  (`ok-enum-field`, `exe-enum-field`).
- [x] Nested type arguments close without a space: the type list stops
  when a `>` is banked, the guard the turbofish path already had, so
  `a: Vec<Vec<i32>>` parses (`ok-nested-generics`,
  `exe-nested-generics`).
- [x] The LLVM textual-IR emission under the wasm-host test suite: the
  runtime helpers' shapes were the 64-bit spelling of pointer-width
  values, and with the canonical `i64` shapes and the runtime's own
  widening, `emit_mode_test`'s "writes the module as textual IR" and
  the CLI's same case pass under the emscripten host.
- [ ] The tree-sitter grammar reads a condition that opens with `(` as
  a pattern, so `if (x) == (y) {}` and `if (x, 1) == (y, 1) {}` parse
  as an error there while the compiler accepts them. The
  structural-equality cases bind their tuple comparisons first to keep
  the differential check green; a condition grammar that forks on the
  token after the closing `)` is what would let the inline form
  through.
- [x] Qualified generic calls: `helper::id::<i32>(x)` answered
  "Unresolved value 'id'" because neither module-qualified resolution
  branch reached the generic-function lookup. Both do now, with the
  reachability check a qualified name owes (`ok-qualified-generic-call`,
  `err-qualified-generic-private`).
- [x] An associated function's turbofish names its own arguments: the
  parser records which segment the turbofish followed and the resolver
  reads it, so `P::id::<i32>(x)` instantiates the member while
  `P::<T>::id(x)` names the owner's (`ok-assoc-fn-turbofish`).
- [x] Read-before-write through an operator: an assignment reads its
  value before it takes the place, so `v[i] = v[i] + 1`,
  `m[k] = m[k] + 1`, and the compound forms compile without a
  temporary. Two-phase borrows are still the general answer for a call
  that both reads and writes.
- [x] A returned error from `main() -> Result` is an exit status: the
  message goes to stderr and the process exits 1, matching the direct
  backend, where LLVM's entry used to call `alcy_panic` and abort with
  status 134.
- [x] Structural equality for tuples and arrays: `==` and `!=` walk the
  elements when every element is itself builtin comparable, which is the
  rule `docs/spec/types.md` already stated.
- [x] Float ranges: `for x in 0.5..<2.5` answers one diagnostic ("Float
  ranges do not iterate"). The arm of the desugared `Some` match binds
  its names against the error type now instead of being skipped, so the
  loop body no longer follows with an unresolved name (or a variant
  that "is not a member of the matched type"). `err-float-range` pins
  both halves.
- [x] A borrow of a temporary works (`a.eq(&2)`, `takes(&format("y"))`,
  `format("x").to_ascii_lowercase()`): the lowering gives the value a
  slot in the frame and loans that, and an owning one becomes an
  unnamed local the drop walk ends at the enclosing block, so no
  reference outlives it. `exe-borrow-temporary` and
  `exe-borrow-temporary-owning` walk the shapes under the sanitizer.
- [ ] `Option::is_some`/`is_none`/`is_ok`/`is_err` take the receiver by
  value, so the option cannot be consulted twice. `&Self` receivers
  would fix that.
- [x] Standard dependency selection: the overlap diagnostic and the
  `--deps` help name the default `alcy/std/*` glob and the rule, so
  `--deps alcy/std/collections` explains itself: name a package or a
  member, not both.
- [x] `Map::get_or_insert(key, value)` returns the entry as `&mut V`,
  inserting first when the key is absent, and a missing key in `m[key]`
  panics with a map-specific message instead of the `Option::unwrap`
  wording.
- [ ] `Map` key ergonomics, the rest: an `Entry` once the borrow rules
  allow, generic keys, which wait for a `Hash` spec, and
  bounds; `keys()` traversal and `Vec` sorting moved to the roadmap's
  iteration and operator items.
- [x] Trace report nesting: the scan kept only the interval on top of
  the open stack, so an interval a later event spilled out of was gone
  before a shorter event that still fitted inside it arrived, and two
  threads reshaped the tree. The parent is the smallest span among the
  open intervals containing the event now, which one thread cannot tell
  apart from the old rule.
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
