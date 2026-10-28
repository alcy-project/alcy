# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

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

- [ ] A generic whose signature names an owning function type cannot be
  checked once, because the closure's witness is per instantiation
  (ADR-0075): the type the body returns is not the type the declaration
  names. Making the closure's type carry its own parameters is what
  would let such a generic be checked once, and cross a package
  boundary with the rest.

- [ ] The generic boundary in a summary: a `needs_body` row is
  omitted, so a dependent that names a generic item reads "Unresolved
  import" without learning why. The IR has no parameterized types (its
  tags are concrete), so a declaration's shape cannot be spelled; it
  exists only per instantiation. The decision is ADR-0079: a generic
  item crosses as its declaration and its checked body, and the
  dependent instantiates it. What remains is that work - a type
  parameter in the IR, the body checked once against its parameters, the
  specialization pass, the body in the summary, and the AST-level
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

- [ ] Split `Lowerer`: comp evaluation, format expansion, and AST-to-IR
  lowering share one class with a large public surface.

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
