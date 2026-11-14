# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

- [ ] A pattern binding of a non-`Copy` payload double-frees when the
  arm's test fails: the binding loads the payload out (the arm's test
  block binds before it tests) and no event records where the load
  read from, so the walk ends both the binding and the scrutinee's
  field — the same buffer — at their scope exits. A guarded end on one
  place now latches its flag after it runs, which fixed the shape where
  the two ends shared one flag; the loaded-payload shape still owes the
  event the load owes its source
  (`e2e/cases/ok-pattern-binding-after-test` and
  `exe/cases/exe-match-guard-discard` abort in the allocator).

- [ ] A match whose pattern binds nothing refuses a program that moves
  nothing: `match s { S { items: _, tags: _ } => ... }` on an owned
  non-`Copy` local answers `Use of moved` although the arms end the
  fields in place. The arms' part ends mark the parts moved and the
  join's whole-scrutinee marker then overlaps them; the enum-payload
  form answers correctly, so the reconciliation is between the join
  marker and the ends the arms emitted. The struct and tuple forms of
  `exe-match-discard-end` wait on this one.

- [ ] Float `**` is refused while the integer one is repeated
  squaring: a fractional exponent needs `pow`, and the direct-wasm
  target imports no libm and has no instruction for it, so admitting
  floats on one backend only would make the same program compile
  differently per target. A portable story wants either an inline
  exp/ln pair the std owns or a wasm import the freestanding target
  agrees to carry.

- [ ] Moving an element out of an owned array, which the refusal above
  stands in for: the drop walk reads a constant index as a path step, so
  a store ends the element it replaces, but a move-out needs an end for
  the elements the move leaves behind, which the loop that ends a whole
  array does not know. A value moved out of a temporary array
  (`f().a[0]`) is the same shape one level out. The refusal reports on
  its own: it ends lowering for the refused expression, so a projection
  through the binding taken from it stays silent
  (`err-move-out-of-element`).

- [ ] The compiler's stack on a small-stack host: compiling to an
  object needs most of 64 KiB even natively, so the wasm test binary
  runs with a 1 MiB stack while programs it builds keep the default.
  Any standard library growth showed up as a check failure rather than
  a number; find the deep path, or budget the stack, before the
  headroom runs out the same way.

- [ ] Codegen at the default optimization level costs seconds for a
  function with many constant-size fills: 400 arrays of `[0u8; 64]`
  reach an object in 16 s and 843 MiB, against 1.9 s and 624 MiB for
  400 arrays of eight `u16` elements written through element stores, a
  quarter of the bytes. The per-fill cost falls as the count rises, so
  the curve is linear with a large constant rather than quadratic;
  where the time goes inside `llvm.memset` expansion at `-O0` is
  unmeasured.

- [ ] The closure's parametric half (ADR-0116, ADR-0079 slice two):
  the spelling is bound to `core`'s `Fn` by identity, a hand-spelled
  `Fn` bound instantiates, and a call on a closure nominal is a direct
  call to the function its literal registered. What remains is the
  once-checked body: the literal interns its captures under the
  declaration's type parameters, the return slot binds once instead of
  per instantiation, the stand-in and the per-instantiation witness
  record retire, and a call the parameter's type decides records a
  dispatch the specialization resolves (ADR-0080).

- [ ] The IR resolves its semantic operations in place, step by step
  (the multi-level direction, MLIR-like without rebuilds): a construct
  whose meaning needs more than its types waits for the pass that
  knows them - dispatch and drops for the specialization pass - and
  later passes patch what they resolve instead of rebuilding the IR.
  Enum projections and a copy's alignment already resolve at emission;
  that is the boundary's first shape. The decision
  (`docs/adr/0079-a-generic-item-crosses-as-its-checked-body.md`)
  started the boundary informally. Generalize it, and decide whether
  the borrow pass moves to the semantic side and whether fine-grained
  in-place patching needs more than `take_state` + `StorageBuilder`,
  when the profile asks.

- [ ] `core::string`'s `trim_start_matches` once reported `Returns
  reference to local 's'` for the `None` arm's `ret s` - the loop
  reassigns `mut s` from `s.strip_prefix(pat)` and the local holds a
  slice view, so the loan's place is the local while its pointee is the
  text. Six shapes faithful to it pass now (the free function with one
  and two parameters, the method form, the staged prelude reached from
  a program, and both reassignment orders), so the function alone does
  not produce the report; it came from a configuration that no longer
  exists (a summary call declared as a bodyless foreign function, which
  reached std code the extern path never reaches) and rebuilding that
  route is what would say whether the report was a false positive about
  a loan's place or a conservative rule worth keeping.

- [ ] The embedded linker beyond this host: startup discovery hard-codes
  Linux paths, only the ELF driver is embedded, and ADR-0049's M3 (the
  wasm lld driver and the portable full compiler) is unstarted.

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

- [ ] Borrow-pass scratch sized by the program, not the function: the
  arrays are allocated at package size and each function clears only the
  entries it names. The final pass's per-worker memory cost is why
  spreading it lost by measurement (ADR-0047), so the sizing waits for a
  reason to spread it.

- [ ] Two handles over one exclusive loan: a closure that owns nothing
  is not moved (its captures are borrows, ADR-0064), so `h := g` on a
  closure holding a `&mut` capture gives two handles that share one
  loan, which `exe-closure-capture-mut` expects; whether both may write
  through it is the borrow checker's question rather than the move
  walk's.

- [ ] The exponent form's mantissa is exact to fifteen significant
  digits: the fixed form takes every digit from an exact conversion,
  while the exponent form scales the magnitude by a power of ten the
  table holds and each product of that scale rounds, so the sixteenth
  digit carries to within a unit and the seventeenth to within a few
  ulp (`{:.16f}` of `1.4e-323` is exact, `{:.16f}` of
  `5.1806537865363085e-318` prints `5.180653786536308e-318` against
  `...309`, and `{:.17f}` of `5e-324` prints `4.94065645841246504e-324`
  against `...544`). An exact mantissa wants an error-free product
  carried beside the rounded one, a scaled long division; the spec's
  floor of about sixteen significant digits stands meanwhile.

- [ ] The direct-wasm stack budget: recursion deeper than the reserve
  holds panics with "stack overflow" (ADR-0141), a per-target limit
  native does not share and no spec states. The frame a statement with a
  slot or a constant-count alloca takes is never returned either, so a
  function's frame grows with the statements it spells, and the reserve
  it feeds is the sum over the whole program rather than its deepest
  chain.

- [ ] Owning function types (ADR-0075) beyond parameters and returns:
  a method's `fn(...)` parameter infers from its arguments, a turbofish
  spells only the parameters a declaration writes, and the return
  position takes its witness from the body's first `ret` - free
  functions and methods alike (`ok-own-fn-return`,
  `exe-own-fn-return`, `err-own-fn-return-mismatch`,
  `err-own-fn-return-missing`, `ok-own-fn-turbofish`). What remains: a
  spelling nested inside another type (`-> Option<fn(i32) -> i32>`, a
  struct field) still refuses (`err-own-func-type`); and named storage
  (`struct H { f: fn(...) }`) needs generics threaded by hand.

- [ ] `Map` key ergonomics, the rest: an `Entry` once the borrow rules
  allow, generic keys, which wait for a `Hash` spec, and
  bounds; `keys()` traversal and `Vec` sorting moved to the roadmap's
  iteration and operator items.

- [ ] A mutator that answers `bool` is discarded by a plain
  statement with a warning: `Set::insert`, `Set::remove` and
  `Map::insert` report what they did, and `a.insert(k)` alone throws
  the answer away. `#[must_use]` on the return, or a spelling that
  names the discard, is the decision; it wants one answer for the
  whole std rather than a `_` at each call site.

- [ ] A value's type is named in three places, and a place is not a
  value: a register's row, the operand that names it, and the
  lowering's own value each carry a type. The verifier compares the
  first two for a primitive tag, where a disagreement writes a word of
  the wrong width; the two may name different indexes of that tag,
  since a type index can be resolved in place. Every other tag stays
  uncompared, because a place's address is a pointer value while a
  register that holds one is declared with the pointee's type, so the
  IR cannot say what such a register holds. The direct-wasm backend
  reads the operand's, which is why the borrowed binding's fix
  restamped one operand to match, and the backend's `is_address` case
  and the `Val` constructions around `lowering_expr.cc:377` are the same
  gap. One home for a value's type, and a place that is not a value, is
  the fix: a register that declared whether it holds an address or the
  pointee would let the check cover every tag.

- [ ] The direct-wasm backend still has no allocation model for the
  frame: a value with a slot and every constant-count alloca keep their
  storage until the function returns, and allocas need a second frame
  pass, so a frame grows with the statements a function spells rather
  than the values live at once, and the stack guard costs four or five
  instructions per call in functions whose frame cannot reach the limit. The opcodes are raw hex whose only name
  is a comment, so a wide shift was misencoded while the suite was
  green, and a narrow result is canonicalized per operation rather than
  at one destination rule.

- [ ] A closure's environment is the closure's own address, so the
  value is self-referential and cannot be moved: a store of the address
  of a moved place is refused, and a closure that owns
  nothing is declared not moved, which is not a property of its type -
  a `&mut` capture is an exclusive loan, and two handles over one
  loan are what `exe-closure-capture-mut` expects. The environment
  belongs outside the value, or beside the call as its own operand.

- [ ] ADR-0076 says the region inference is total, and the backlog
  holds a program that reads dead storage and compiles. Until the
  solver exists, that record is a claim the code does not make, and a
  checker that accepts incorrect code is worse than one that refuses
  correct code.

- [ ] A summary identifies a nominal by comparing two type graphs, so
  two declarations with the same shape are the same type: the matcher
  walks both graphs with a pair budget, and a declaration past it, or
  one that two rows match, is dropped without a word. A declaration
  identity in the artifact, and a comparison by it, would end the
  matcher, the name fallback, and the per-occurrence re-spelling.

- [ ] A summary carries no spec implementation, so a dependent cannot
  see that a library's type implements one, and the roadmap asks a
  summary to carry them.

- [ ] `f64::parse` rounds correctly only while the digits and the
  factors of five the decimal places and the exponent need fit one
  `u64`, which holds five twenty-seven times:
  `"0.00000000000000000001"` reads, while
  `"18446744073709551616.0"`, `"56748e22"` and `"1e400"` are refused
  rather than read. Reading them wants the conversion to run over two
  words or more, and the language has no arithmetic wider than `u64` yet
  (`u128` is deferred). The exponent folds into the numerator or the
  scale as factors of five with the twos riding the binary exponent, so
  `6.02e23` and `3.9577e-16` round once from exact digits, while
  `1e308` and `1e-400` are refused where a two-word numerator and
  scale would read them.

- [ ] A value that carries a container's buffer loan cannot leave the
  function that owns the container: `fn take() -> Option<String> { mut v
  := Vec::<String>::new(); v.push(String::from("a")); ret v.pop() }`
  reports `Move of 'v' invalidates an outstanding borrow` and `Returns
  reference to local 'v'`, and binding the answer to a local first and
  returning that local reads the same way. The scalar-aggregate
  relaxation keeps the loan for a payload that carries a reference, so
  a `Copy` element survives (`ok-borrow-owned-element`) and an owning
  one keeps reading as borrowed from the container
  (`err-borrow-live-owned` pins the `push` conflict); handing it back
  needs the region a loan carries rather than a fixed range, which
  ADR-0012 records as the work that replaces this.
  `check_borrow_rules.py` tracks the shape
  (`returned/owned-element-returned`).

- [ ] A callee written to take its argument as a `str` can store the
  view it was handed: `fn store(mut v: &mut Vec<str>, s: str) {
  v.push(s) }` called as `store(&mut v, x.as_str())` puts a view of
  the caller's slot into the caller's container, and the program then
  prints the dropped buffer. The caller cannot see it, because `str` is
  the spelling a copying callee uses too (`String::push_str`,
  `Map::insert` and `Set::insert` all copy the bytes), so the
  declaration separates nothing: what separates them is a disposition
  per parameter - kept or copied - which is the region a loan carries
  (ADR-0012) read off the reasoned list of the std surface (ADR-0131).
  The extent rule covers the rest of the shape: a container declared
  before the view's buffer is refused inside one frame
  (`err-kept-view-inner-block`, `kept/str-view-from-inner-block`) and
  accepted when the buffer is declared first
  (`kept/str-view-from-outer-block`), a place that kept a view of a
  slot ending with its block is refused when the block hands it out
  (`kept/str-view-in-block-value`) and when the frame returns it
  (`kept/str-view-returned`,
  `returned/borrowed-element-in-container`), and the same view of a
  parameter or a call result is accepted
  (`kept/str-view-of-parameter-returned`,
  `returned/borrowed-element-from-parameter`). The copying receivers
  stay accepted: `ok-mini-pipeline` and `ok-grouping` pass views to
  them.

- [ ] The cache is consulted after the frontend has parsed and
  resolved, so a hit cannot skip the work that dominates, and the
  compiler's version string does not identify the binary that wrote an
  entry: two builds of one dirty tree share every key.

- [ ] A reference to a slice is fat, so an option over one carries a
  discriminant (ADR-0087 promises no niche there). The data pointer's
  null pattern is still spare: teach the niche a fat null - a null pair
  as the fieldless spell, and the discriminant read through the data
  pointer - and `Option<&[T]>` is two words again.

- [ ] Compile-time reflection, read-only: the blocker for the struct
  flavor of input, a clap-like argument parser, and serde-like
  (de)serialization (ADR-0129, ADR-0130). Its record lands with the
  design, after expansion; the floor is read-only and comp-only, with
  no runtime type information and no generative types.

- [ ] `fmt` with no file argument formats the package in the current
  directory, and only its predicate says so: `check_e2e.py` appends a
  positional to every case, and a unit test would have to move the test
  process into the package, which formats the repository when the run
  happens under a manifest. A runner option that sets a case's working
  directory pins it the way the other package verbs are pinned.

- [ ] A failing test says so by panicking, so an expected failure is
  spelled as a panic and a run cannot tell a test that expected to fail
  from one that broke. An entry with a return type - `Result<(), E>`, the
  form the entry point already takes - would let a run report `Err` as a
  failure in the error's own words, and let a test keep the error it
  expects rather than aborting on it. ADR-0009 keeps `Result` an ordinary
  `core` enum, so the run would have to read a type it does not own, and
  a package that shadows the name would change what the run reads.

- [ ] The doctest files under `src/tests` hold two kinds of case, and
  only one of them belongs there. A case that asserts on a program which
  should work - a snippet the pipeline accepts, a borrow the checker
  allows, a value that lowers to the shape it should - is a statement
  about alcy source, and it is written in C++ only because the standard
  library had nowhere to state its own behaviour until `alcy test` could
  compile a block. Such a case goes in a `test` block in the std module
  whose behaviour it describes, so `alcy test lib/std/<member>` runs it
  with the same command a user runs, over the source the module ships: a
  case that sits beside the code it describes cannot drift from that
  code's surface without the run failing. A case that asserts a program
  should be refused - a diagnostic, a rejected form - stays, because a
  refusal is only observable from the outside. What the doctest keeps of
  the rest is the case about the compiler's own API, which no `test`
  block can address.

- [ ] A run starts one test process at a time, so a package's tests cost
  the sum of their times. `-j N` would start up to N of them and still
  report in selection order, which is the order a reader diffs against the
  previous run; the loop is already one index at a time, so what is
  missing is the state a parallel loop needs. The child inherits the
  runner's stdout, so concurrent tests would interleave their output, and
  the report would have to hold a test's lines until its status arrives
  rather than write as it goes.

- [ ] `--execution=multi-thread` runs a package's tests as threads of one
  process, which is what a suite of cheap tests wants: no spawn, no
  separate address space. It waits for unwinding to exist. A test fails by
  aborting and an abort ends the process, so today the run can offer
  either one process per test or one process for all of them, and
  `--execution=single-thread` exists for the second because a debugger
  needs it, not because a suite does.
