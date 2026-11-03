# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

- [ ] A suite's manifest is read once per suite edge, not once per
  closure: the staged-dependency table keys directories that loaded as
  packages, and the suite root - which loads no package - is not in
  it, so two edges into one suite read and re-parse its manifest. The
  entry the table keeps is a dependency subtree; a suite root has
  none.

- [ ] The run-after-build cache hit is the promise the run cache
  makes, and nothing pins it across processes: the key covers the
  loaded-file table, so a build and a run that load different
  auxiliary files (toolchain, profiles, a suite manifest) silently
  miss instead of hitting. A test that builds in one process and runs
  in another, asserting the JSON `cache` field, would name the
  failure.

- [ ] Float `**` is refused while the integer one is repeated
  squaring: a fractional exponent needs `pow`, and the direct-wasm
  target imports no libm and has no instruction for it, so admitting
  floats on one backend only would make the same program compile
  differently per target. A portable story wants either an inline
  exp/ln pair the std owns or a wasm import the freestanding target
  agrees to carry.

- [ ] Moving an element out of an owned array, which the refusal above
  stands in for: the drop walk's paths name fields and tuple slots, so
  the pass needs an element step that ends the elements the move left
  behind. A value moved out of a temporary array (`f().a[0]`) is the
  same shape one level out. The refusal reports on its own: it ends
  lowering for the refused expression, so a projection through the
  binding taken from it stays silent (`err-move-out-of-element`).

- [ ] The compiler's stack on a small-stack host: compiling to an
  object needs most of 64 KiB even natively, so the wasm test binary
  runs with a 1 MiB stack while programs it builds keep the default.
  Any standard library growth showed up as a check failure rather than
  a number; find the deep path, or budget the stack, before the
  headroom runs out the same way.

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

- [ ] The generic boundary in a summary: a `needs_body` row is
  omitted, so a dependent that names a generic item reads "Unresolved
  import" without learning why. The IR has no parameterized types (its
  tags are concrete), so a declaration's shape cannot be spelled; it
  exists only per instantiation. The decision is ADR-0079: a generic
  item crosses as its declaration and its checked body, and the
  dependent instantiates it. The IR's type parameter and the body
  checked once landed (ADR-0079 slices one and two); what remains is
  the specialization pass, the body in the summary, and the AST-level
  instantiation walk deleted.

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

- [ ] Raise the lowering's function budget. `MAX_FN_ENTRIES` caps the
  functions one package may lower at 8192, and a package that needs more
  is refused with `ED013` where the last slot is spent. Nothing
  measures what a larger number would cost: the summary walks every
  entry once, and the per-function and per-instantiation caches are
  package-sized, so the cap moves on a measurement rather than a guess.
  The side tables that made the scan-based readers expensive are indexed
  now, which is the part the proportionality work (ADR-0046) closed.

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

- [ ] A spec method's *receiver* bound is still unchecked: a
  declaration stating `fn take<U: PartialEq>(self: &Self) where Self:
  Ordered` has its `U` checked at the call (`check_spec_method_bounds`,
  `757074b3`) and its `Self` not, because the impl fixes the receiver
  when it is written and nothing compares the two. The helper already
  binds the specification's parameters; the missing piece is a check
  over `declaration.params` in the bounds the impl recorded.

- [ ] A value's type is named in three places, and a place is not a
  value: a register's row, the operand that names it, and the
  lowering's own value each carry a type, and the verifier compares
  none of them. The direct-wasm backend reads the operand's, so a
  lowering site that disagrees with the register writes a word of the
  wrong width - the borrowed binding's fix restamped one operand to
  match, and the backend's `is_address` case and the `Val` constructions
  around `lowering_expr.cc:377` are the same gap. A place's address is a
  pointer value, but a register that holds one is declared with the
  pointee's type, so the IR cannot say what the register holds. One
  home for a value's type, and a place that is not a value, is the
  fix; the verifier's silence is what let it drift.

- [ ] The direct-wasm backend has no allocation model: every SSA
  register gets a wasm local of its own, against ADR-0049's slot model,
  so the local count is the register count and one inlined call crosses
  the engine's cap, allocas need a second frame pass, and the stack
  guard costs four or five instructions per call in functions whose
  frame cannot reach the limit. The opcodes are raw hex whose only name
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

- [ ] A single file checked or compiled on its own parses the target as
  an unnamed root beside the embedded suite, so a standard-library
  source handed to `--file` is not the member it copies. `alcy check
  --file lib/std/core/string.al` reports `Type mismatch in method call:
  expected 'StrIter', found 'StrIter'` (exit 4), because the root copy's
  `impl str` declares its own `StrIter` while the call resolves to the
  embedded member's, and two declarations print one spelling. Eighteen
  of forty suite sources check cleanly, so refusing the form is not the
  answer. Two shapes were tried and each leaves a hole: naming the root
  after the member its path ends with drops the embedded copy, and then
  `core/prelude.al` cannot resolve `package::string`, the target sitting
  in the root package instead of `core`; swapping that member's input
  for the checkout's bytes pairs one file into two modules, and the
  root pairing cannot resolve the file's own `is_ascii_space`. The fix
  is a single-file mode that loads the target as the member it is, in
  the member's package, with that module as the root of the analysis
  rather than a root package of its own.

- [ ] `f64::parse` rounds correctly only while the digits fit one
  `u64` numerator, so `"18446744073709551616.0"` and
  `"0.00000000000000000001"` are refused rather than read. Reading
  them wants the conversion to run over two words or more, and the
  language has no arithmetic wider than `u64` yet (`u128` is
  deferred).

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
