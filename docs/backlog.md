# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

- [ ] The return-escape rule exempts a loan by its root's type tag: a
  `str`-typed root is a view, so a loan rooted at one is taken to ride
  the buffer the view reads. The tag cannot tell that loan from one a
  `&u` spelling creates on the view's own slot, so `fn leak() -> &str {
  u := "abc"; ret &u }` compiles and hands the frame's two words back -
  main refuses it with EF003, and the exemption landed with the
  str-view loans (`ddeb3e40`) because the std's own bodies rebuild a
  local `str` from a parameter and hand it back. The provenance the
  rule needs - whether the loan names the buffer the view reads or the
  view's own slot - is what the store/load chain through the local's
  alloca does not carry; the two shapes a fix takes are a loan born at
  a borrow of the slot refused at the return, or the view's buffer
  provenance carried through the copy. `ret &GREETING` for a `str`
  const is the same hole through the const temporary.

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

- [ ] A reference stored into a longer-lived place outlives its
  referent: `mut r: &i32 := &0; { n := 5; r = &n; } *r` compiles, and
  so does the same store through a struct field or an `if` arm, which
  leaves `*r` reading storage the scope already ended.
  `docs/spec/deferred.md` leaves the outlives constraints between
  regions to the region-annotated captures work, so the check wants that
  relation rather than a rule about one instruction; the case is here so
  that work starts from a reproducer.

- [ ] A type mismatch between two closure literals reads `expected
  'struct', found 'struct'`: each closure literal is its own type, which
  is what the error is about, and the message names the kind both sides
  share instead. Spelling an anonymous closure by its signature, or
  numbering the environments, is what a reader needs - it wants a
  decision on what a closure type is called in a diagnostic.

- [ ] An unreachable match arm is not reported: two arms with the same
  literal pattern keep the first and the later one can never run, while
  the compiler does warn about a statement after `ret` (WD003). The
  complete check is pattern overlap - ranges, or-patterns, enum shapes -
  and wants a decision on whether the duplicate is a warning or an
  error; a guarded arm must not count as covering a later unguarded one.

- [ ] The exponent form's mantissa is reached by multiplying the
  magnitude by ten or a tenth per step, so its low places are the
  accumulated rounding, not the value's: `{:.17f}` of `f64::MAX` prints
  `1.79769313486234683e308` where the value rounds to
  `1.79769313486231571e308`, about thirteen good places out of the
  seventeen asked for. A single scaled multiplication per step would
  bound the error to one rounding, and the exact answer wants a power of
  ten table or the scaled long division the fraction digits use.

- [ ] Moving an element out of an owned array, which the refusal above
  stands in for: the drop walk's paths name fields and tuple slots, so
  the pass needs an element step that ends the elements the move left
  behind. A value moved out of a temporary array (`f().a[0]`) is the
  same shape one level out.

- [ ] Negative comp-arg cases for the loosened predicates: a runtime
  value inside a match arm or a block (`double(match runtime_val …)`,
  `double({ y := runtime; y })`) must still be refused, the guard
  check a match now performs wants its own pin, and a generic caller's
  other-instantiation variant record wants one case proving the
  predicate's miss stays a compile error. The spelling and the
  evaluation rules are in `docs/spec/comp.md`; the predicate lives in
  `src/analyzer/comp_evaluator.cc` with the interface in
  `comp_evaluator.h`.

- [ ] The compiler's stack on a small-stack host: compiling to an
  object needs most of 64 KiB even natively, so the wasm test binary
  runs with a 1 MiB stack while programs it builds keep the default.
  Any standard library growth showed up as a gate failure rather than
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
  knows them - enum projections and `memcpy` alignment for the
  backend, dispatch and drops for the specialization pass - and later
  passes patch what they resolve instead of rebuilding the IR.
  ADR-0079's landing started the boundary informally; generalize it,
  and decide whether the borrow pass moves to the semantic side and
  whether fine-grained in-place patching needs more than
  `take_state` + `StorageBuilder`, when the profile asks.

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

- [ ] Index the side tables the proportionality work left behind. Most
  landed (ADR-0046); the tail is `expr_types` in both readers, the
  per-module closure, closure-fn, and indirect-call tables,
  `instantiate_fn`'s instance scan, `fn_args_for` and `closure_fn_index`,
  and the module statics. `Lowerer::expr_type` scans backward ("last
  entry wins") while `CompEvaluator::expr_type_in` scans forward ("first
  wins"), so the readers can disagree on a duplicate key, and
  `MAX_FN_ENTRIES = 8192` reports `ED013` at the call that spends the
  last slot now, rather than failing at an unspecified internal span;
  raising the budget itself waits for the indexing.

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

- [ ] `Map` key ergonomics, the rest: an `Entry` once the borrow rules
  allow, generic keys, which wait for a `Hash` spec, and
  bounds; `keys()` traversal and `Vec` sorting moved to the roadmap's
  iteration and operator items.

- [ ] Set algebra: `union_with` and `intersection` build a new set from
  two, and the cases for them were written before either existed. The
  set carries `insert`, `remove`, `contains`, `is_subset` and
  `extend` over `str` keys, and a builder returns `bool`, which a
  statement discards with a warning. The two operations want the same
  answer: whether a builder's result is the caller's to ignore.

- [ ] `String::insert_str` at a byte index, which `insert_byte` does for
  one byte. The index must be a character boundary, and nothing yet
  spells that refusal, so the operation waits on the boundary check
  `str` slicing shares.

- [ ] A spec method's declared bound is never checked at the call. The
  declaration says `fn check<U: PartialEq>`, an implementation may
  leave the bound out, and `spec_method_sig` binds the method's type
  parameters to the call's arguments without asking whether those
  arguments satisfy what the declaration requires. `check_declared_bounds`
  answers the question for a free function and for a generic impl's
  own parameters, so the missing piece is the declaration.

  The call does not reach `lookup_method`'s generic branch for a
  concrete impl: `generic_record` is set only when the impl's *target*
  names a parameter or a value, so `impl Checkable for P` takes the
  other branch, which hands back the registered method and never asks
  the declaration anything. Registering that method compares the two
  signatures under one stand-in per parameter, and equal stand-ins
  match, so neither side's bounds are consulted. A fix has to put the
  question where both branches meet - the check belongs after the
  candidate is chosen and before it is returned - and it has to
  resolve the receiver's arguments from `entry.spec_args` under the
  impl's scope, the way the generic branch already does.

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

- [ ] `f64::parse` converts the integer part exactly and still divides a
  fraction below one in f64, so a numerator past 2^53 rounds twice:
  `"0.9876543210987654321".parse_f64()` answers 0.98765432109876550
  where the literal `0.9876543210987654321` answers
  0.98765432109876539, one step apart, and `"0.3333333333333333333"`
  and `"0.6666666666666666667"` are the same shape. An integral value
  is exact now (`"9007199254740993.0"` answers 2^53), so the hole is
  the fraction whose numerator passes 2^53 with no factor of ten to
  cancel; closing it wants a rounding decision over more than the u64
  numerator, and the language has no wider word.

- [ ] A request the allocator cannot meet is a panic in an ordinary
  build and a sanitizer report in an instrumented one:
  `Vec::<u8>::with_capacity(18446744073709551615u64)` prints `panic:
  allocation failed` and exits 134, while the same program under the
  address sanitizer stops inside `posix_memalign` with `requested
  allocation size ... exceeds maximum supported size` before `alcy_alloc`
  answers, so the instrumented gate cannot carry a case for it.
  `doubled_capacity` refuses the doubling that would not fit a `usize`;
  a guard over the byte count before the host allocator is what would
  let the two builds say the same thing.

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

- [ ] A reference to a container's element carries no loan: `r := &v[0]`
  held across a `v.push` is accepted, the growth frees the buffer the
  reference reads through, and the address sanitizer reports the
  use-after-free the checker missed. `index_mut` is a spec method and a
  summary carries no spec implementation, so the callee's return-region
  never reaches the caller and the receiver's loan dies at the call;
  the borrow on the returned reference names no place either, the
  element sitting behind the container's raw buffer pointer. The
  runtime half is covered now - the instrumented gate reports the read
  - and the checker's half waits on the summary work above.

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
