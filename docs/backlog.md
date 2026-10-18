# Backlog

Peripheral work, unordered. When every item is checked the list is
replaced. The foundations are in `roadmap.md`.

## Open

- [x] The exhaustiveness check read one level, so `Some(Some(v))` and
  `None` covered `Some` and `None` and `Some(None)` kept no arm: the
  match compiled and the value fell through - a segfault on the host and
  a trap on wasm, with the same hole behind a tuple, a struct field, and
  a nested variant payload. A bare name that spells a fieldless variant
  counted as a binding for the same reason. `patterns_cover` specializes
  one column at a time now, and a row of wildcards ends the descent on a
  type that contains itself. `exe-nested-pattern` and
  `ok-nested-pattern` each held a match genuinely missing `E::C`.
- [ ] `size_of::<T>()` is not comp-evaluable: `const N: usize =
  size_of::<()>()` reports `Construct 'intrinsic is not comp-evaluable'
  is not supported`. The answer is a property of the layout the lowering
  already holds, and an array length written with it would fold. It wants
  a decision on which layout queries the comp engine may answer.
- [ ] A type mismatch between two closure literals reads `expected
  'struct', found 'struct'`: each closure literal is its own type, which
  is what the error is about, and the message names the kind both sides
  share instead. Spelling an anonymous closure by its signature, or
  numbering the environments, is what a reader needs - it wants a
  decision on what a closure type is called in a diagnostic.
- [x] The wasm call site pushed each argument's own words, and the
  operand a `()` argument carries is the lowering's placeholder, so the
  placeholder stayed on the stack and the next store read it as its
  address - the call ran, returned the right value, and wrote elsewhere.
  Arguments are matched to the callee's parameter types now, and a
  zero-sized one contributes nothing. A zero-sized load or store was
  also `Construct 'void' is not supported`, which kept `Vec<()>` from
  compiling at all; both are no-ops. `e2e/wasm/unit-arg` holds the
  shapes and `exe-unit-arg` runs them on the host.
- [x] The uninhabited type reached LLVM as `void` wherever it appeared,
  so the same data layout assertion that caught the unit type caught a
  `!` field, a `!` tuple element, a `!` parameter, and a `!` array
  element - the layout gives every one of those size zero. It builds as
  the empty struct now, and the `ret` guard accepts both zero-sized
  tags. `exe-never-shape` holds the shapes.
- [x] The unit type was LLVM's `void` wherever it appeared, so a `()`
  field, tuple element, or argument crashed the emitter on the data
  layout's assertion that a type have a size, while the wasm backend
  compiled the same shapes. It builds as the empty struct the layout
  describes now, a function's return position keeps `void`, an argument
  carries the empty struct its parameter names, and a store through a
  pointer to the unit type writes nothing - without that last part
  `h.a = ()` put eight placeholder bytes over `h.b`, which the layout
  places at the same offset. `exe-unit-field` holds the shapes.
- [ ] An unreachable match arm is not reported: two arms with the same
  literal pattern keep the first and the later one can never run, while
  the compiler does warn about a statement after `ret` (WD003). The
  complete check is pattern overlap - ranges, or-patterns, enum shapes -
  and wants a decision on whether the duplicate is a warning or an
  error; a guarded arm must not count as covering a later unguarded one.
- [ ] A UTF-8 byte order mark at the start of a source is an `Invalid
  character` at 1:1: the lexer has no rule for the sequence, so an
  editor that saves one makes the whole file unreadable. Other compilers
  skip it; the spec says nothing, so this wants a decision before the
  lexer learns the three bytes.
- [x] A struct kept two fields under one name and an enum two variants
  under one: nothing at the declaration checked, so a struct literal was
  asked for both fields and an access reached the first, while a second
  variant resolved to whichever entry a lookup reached. The declaration
  site reports the duplicate now, before the generic forms intern per
  use, and `err-duplicate-field` and `err-duplicate-variant` hold both.
- [x] A \u{...} escape whose value is not a Unicode scalar compiled and
  then vanished: the lexer validated the escape's shape only, so
  \u{110000}, a surrogate like \u{D800}, and a digit run past the width
  of the value all passed, and unescaping dropped what it could not
  encode - a string spelled with \u{110000} printed as if the escape
  were not there. The lexer reads the value now, reports it with the
  escape's own span, and the grammar states the range.
- [x] An unsuffixed integer literal in no typed position kept the `i32`
  default whatever it held: `x := 4294967296` compiled to `0`,
  `18446744073709551616` to `0`, and `0x0102030405060708 as u64` to the
  low half of the magnitude. The spec's rule that a literal must fit its
  type ran only for a suffix or an expected integer type, and the
  magnitude parse wrapped past `u64`. The default is checked now, a
  cast's target and the operand beside a literal name the type first, and
  `err-integer-literal-default` and `ok-integer-literal-range` hold the
  two sides.
- [x] The formatter's fixed-point digits were exact only to about
  sixteen places: `{:.Nf}` took each digit by multiplying the fraction
  by ten in `f64`, so the error compounded and the seventeenth place
  was wrong - `{:.17f}` of `0.1` printed `0.10000000000000000` where
  half-up rounding of the value is `0.10000000000000001`, `0.7` printed
  `0.70000000000000000` against `0.69999999999999996`, and `1/3`
  printed `0.33333333333333330` against `0.33333333333333331`. The
  fraction is the dyadic rational `M / 2^K`, and its digits and the
  half-up bit come from one exact product and a shift now, so every place
  is the value's; `exe-fmt-exact` and `e2e/wasm/fmt-exact` hold the
  shapes, and the spec's `N` cap of 17 stands.
- [ ] The exponent form's mantissa is reached by multiplying the
  magnitude by ten or a tenth per step, so its low places are the
  accumulated rounding, not the value's: `{:.17f}` of `f64::MAX` prints
  `1.79769313486234683e308` where the value rounds to
  `1.79769313486231571e308`, about thirteen good places out of the
  seventeen asked for. A single scaled multiplication per step would
  bound the error to one rounding, and the exact answer wants a power of
  ten table or the scaled long division the fraction digits use.
- [x] A cast from a float to an integer was undefined where the language
  says nothing is (ADR-0066): LLVM's `fptosi` left NaN and an
  out-of-range value poison, so a checked build, a release build, and a
  formatter that received the poisoned word each answered differently,
  and the direct-wasm backend trapped. Both backends saturate now
  (ADR-0077), the spec's numeric tower names the rule, and
  `exe-cast-saturate` and `e2e/wasm/cast-saturate` hold the edges of
  every destination.
- [x] Printing a signed type's minimum aborted in a checked build: the
  formatter takes the magnitude with a subtract from zero, which
  overflows for the one value whose magnitude the signed range cannot
  hold, so `println("{}", i32::MIN)` panicked and only a release build
  printed it. The magnitude comes from the wrapping form now, which the
  `MIN / -1` rewrite already uses, and `exe-fmt-min` prints the minimum
  of each signed width.
- [x] A constant comparison folded at the destination's type in the
  release passes: a comparison's destination is a bool, so `-1i8 < 0i8`
  ranked the raw words unsigned and folded to false - an all-constant
  branch in a release build took the wrong arm at every signed width,
  while the debug build, which does not fold, was right. The fold ranks
  the operands' type now, and `exe-fold-compare` holds the pairs.
- [x] A function whose registers pass the wasm local cap produced a
  module no engine parses: the emitter spends one local per register and
  the engines refuse more than 50000, which a function the release
  passes inline a great deal into can reach. The backend reports the
  construct now (ADR-0049), `tools/check_wasm.py` gained an `error`
  field for a case that expects a rejection, and `too-many-locals` uses
  it.
- [x] A frame that reached the direct-wasm stack reserve's end wrote
  itself over the heap: the shadow stack and the bump allocator grow up
  from addresses one reserve apart, so a deep enough recursion replaced
  a live buffer's bytes and the program kept running - a `Vec`
  allocated before the recursion read a frame's own value at its first
  element, and only a recursion that outran the memory trapped. Every
  function compares its frame's new pointer against an immutable global
  the layout publishes and reports "stack overflow" rather than crossing
  it; `e2e/wasm/stack-limit` recurses under the limit with a live vector
  and then past it.
- [x] A raw pointer aborted the direct-wasm emitter at an internal
  check: the element offset `ptr_offset` and `ptr_offset_mut` return
  has a raw pointer destination, and the emit case accepted references
  only. The same program's `(*r).y` asked for a projection through
  `u8`, because a raw pointer base fell to the byte-addressed fallback
  where the pointee is what the walk needs. Both sites dereference the
  pointee the way the LLVM emitter does now, and
  `e2e/wasm/raw-pointer` covers creation, read, write, offset, struct
  projection, and array indexing.
- [x] Narrow arithmetic under direct-wasm left a container that did not
  spell the type: the unchecked form of an `i8` wrap, a left shift, or
  `MIN / -1` kept a value past the type's range, so a compare in a
  release build answered apart from it (`exe-arith-defined` exited 9).
  The arithmetic path canonicalizes a narrow result the way the checked
  path already did, and `e2e/wasm/narrow-arith` runs with `--release`.
- [x] A match over a reference scrutinee wrote its payload binding at
  the pointee's width: the binding's store reused the place operand,
  whose recorded type is the payload's, and a backend that reads the
  operand's type emitted an `i64` store with a pointer word on the
  stack - an invalid module for any `&Enum` with a wide payload. The
  operand restamps itself as the reference now, and
  `e2e/wasm/match-ref-payload` covers the arm.
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
- [ ] `size_of::<T>()` is not comp-evaluable: `const N: usize =
  size_of::<()>()` reports `Construct 'intrinsic is not comp-evaluable'
  is not supported`. The answer is a property of the layout the lowering
  already holds, and an array length written with it would fold. It wants
  a decision on which layout queries the comp engine may answer.
- [ ] A type mismatch between two closure literals reads `expected
  'struct', found 'struct'`: each closure literal is its own type, which
  is what the error is about, and the message names the kind both sides
  share instead. Spelling an anonymous closure by its signature, or
  numbering the environments, is what a reader needs - it wants a
  decision on what a closure type is called in a diagnostic.
- [ ] The compiler's stack on a small-stack host: compiling to an
  object needs most of 64 KiB even natively, so the wasm test binary
  runs with a 1 MiB stack while programs it builds keep the default.
  Any standard library growth showed up as a gate failure rather than
  a number; find the deep path, or budget the stack, before the
  headroom runs out the same way.
- [ ] A deeply nested pattern hangs the cover walk: a `match` arm of
  the form `Option::Some(Option::Some(... (E::A | E::B) ...))` with a
  wildcard arm costs about a second at twenty levels of nesting and
  more than a hundred at twenty-eight, where `alcy check` had to be
  killed. The input is a few hundred bytes, so the cost is in the
  cover walk, not in reading it, and the growth is steep enough to
  read as exponential in the nesting depth. A plain constructor chain
  with no `|` anywhere - `Some(Some(... E::A ...))` and a wildcard
  arm - hangs the same way, so the cost is not the or-pattern
  expansion. Reproduce with `alcy check` on a generated file.
- [ ] An unreachable match arm is not reported: two arms with the same
  literal pattern keep the first and the later one can never run, while
  the compiler does warn about a statement after `ret` (WD003). The
  complete check is pattern overlap - ranges, or-patterns, enum shapes -
  and wants a decision on whether the duplicate is a warning or an
  error; a guarded arm must not count as covering a later unguarded one.
- [x] References into the type tables held across checking: a struct
  literal's check kept a `StructType&` while a field value's check
  could intern a type and move the table (a heap-use-after-free the
  new std module's layout shift exposed). The audit closes with
  forty-five readers copying their entry - the struct, tuple, variant,
  pattern, element-index, run-index, array, intrinsic, and shape
  readers, the drop walk, the comp evaluator, and every other read of
  a builder table a call could move between. The rule is the file's
  now: a reference into a table the checker can grow is never held
  across a call. The few sites that read the frozen `Storage` keep
  their references, because nothing can move it.
- [x] A discarded temporary that owns memory ends where it stands:
  `_ := format("x")` and a bare `format("x")` are both ended by the
  discard path's `record_drop_in_place`, which the sanitized runs of
  `exe-discard-end` confirm. The `fmt` expansion still calls its
  collected tuple's glue directly, because the tuple is a scratch slot
  rather than a binding the pass could schedule.
- [x] A closure that owns its environment keeps its heap: the
  environment's capture type could resolve to a second type node for
  the same nominal (a `String` node beside the declaration's own), and
  the drop glue lookup answered only the type index a destructor was
  registered under, so the closure's nominal read as non-destructible
  and no drop was emitted. The lookup follows the nominal shape now, so
  a node interned through another path ends the way its declaration
  does: `exe-closure-capture-drop` walks a bound closure and a `fn()`
  parameter that both capture a heap string under the sanitizer, and
  the witness in `exe-method-infer` can hold a `String` again.
- [x] The symbol a summary call names, and the linkage that needs: each
  summary row carries the mangled symbol the library's own build writes
  (the emitter's encoding, computed where the artifact is written), the
  dependent's call declares it in the external table, and the target
  links the dependency's object. The runtime's entry points were already
  `linkonce_odr`; the two argument globals are discardable now too, so
  two objects in one image keep one copy. A package whose library has a
  summary resolves, links, and runs with the library's sources gone
  (`dependencies_test` builds both and runs the binary).
- [x] A summary carries no struct field names: the section writes the
  fields each struct row declares - name and visibility, in declaration
  order - and the surface spells a nominal row into its declaration (a
  struct from the runs, an enum from the variant names its table
  holds), so a dependent names the type, constructs a literal, and
  reads a field across the boundary. `dependencies_test` does all three
  with the library's sources gone. A signature that *names* a nominal
  is still left out, and the reason is the item below.
- [x] A summary call passes what the callee takes: the call handed the
  argument's place over where the callee takes its value, so a field
  read as an argument met the emitter's LLVM assertion. It crosses as
  the value now, the way a C entry point's does, and a nominal
  parameter, a nominal return, a nested nominal field, and a reference
  to a nominal all work - `dependencies_test` runs all four with the
  library's sources gone. Spelling a signature through a copy was
  refused while this was open; a row spells it now, because identical
  shapes lay out alike.
- [x] Associated functions and methods in a summary: a row names the
  nominal its item belongs to now (`owner`, the type the declaration
  names), and the surface groups the rows by it into the `impl` the
  declaration's module holds, after every nominal has been spelled. A
  method spells its receiver as `self` - `mut self` when the receiver
  takes an exclusive borrow, which its type says - and its call crosses
  as the value the callee takes with the receiver as the first
  parameter; a call names the callee's own item before its name, which
  two impls could share. A comp function gets no row, because its call
  belongs to the package's build rather than a dependent's.
  `dependencies_test` calls `Pair::new` and `p.total()` with the
  library's sources gone.
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
- [x] One nominal as several type nodes: the checker interned a
  second type node over a declaration's own shape (a `String` node
  beside the declaration's), and `types_equal` compares origins, so two
  nodes over one declaration read as different types while only the
  drop lookup was taught to see through them. The observable symptom
  was a two-node dump (`struct Pair(i32, i32)` at #19 and #158) that
  made a nominal-typed signature unspellable. It no longer reproduces:
  a library with a struct, a nested struct, an impl, and calls dumps
  one node per declaration now, and the dependent spells, links, and
  runs a nominal parameter, return, nested field, and reference. The
  diagnosis that stands is that the object a field or parameter holds
  is a storage copy - a reference the checker records an origin for,
  which lowering follows - so a consumer that meets one resolves it by
  shape; if the extra node returns, the reader audit's rule and
  `type_origins` are where to look.
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
- [x] A moved-from owning closure was still callable: `h := g` handed
  the environment to `h`, but `g()` still passed `g`'s own storage as
  the environment and read what `h`'s drop freed. The borrow pass
  refuses a store whose value is the address of a moved place, which is
  the shape a closure call writes its environment through;
  `err-closure-move-reuse` holds it. A place reached through a deref is
  exempt, because the value names the referent rather than the slot a
  move takes.
- [ ] Two handles over one exclusive loan: a closure that owns nothing
  is not moved (its captures are borrows, ADR-0064), so `h := g` on a
  closure holding a `&mut` capture gives two handles that share one
  loan, which `exe-closure-capture-mut` expects; whether both may write
  through it is the borrow checker's question rather than the move
  walk's.

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
- [ ] A reference stored into a longer-lived place outlives its
  referent: `mut r: &i32 := &0; { n := 5; r = &n; } *r` compiles, and
  so does the same store through a struct field or an `if` arm, which
  leaves `*r` reading storage the scope already ended.
  `docs/spec/deferred.md` leaves the outlives constraints between
  regions to the region-annotated captures work, so the check wants that
  relation rather than a rule about one instruction; the case is here so
  that work starts from a reproducer.
- [x] Two-phase borrows: a call's exclusive receiver is a reservation
  taken before the arguments evaluate, and the call itself activates
  it, so `v.push(v.len())`, `v.push(v[0])`, and
  `v.insert(v.len(), x)` resolve. A read that is still live at the
  activation - a view handed in as an argument, a closure that captured
  the place - still reports (`ok-borrow-two-phase`,
  `exe-borrow-two-phase`, `err-borrow-two-phase-arg`,
  `err-borrow-two-phase-escape`).
- [x] A load of scalars owns its copy: the element load kept the
  receiver's loan alive in a container accessor's answer, so
  `d := v.pop()` on a `Vec<Point>` read as a borrow of `v` and
  refused the next `v.push`. The load now asks the same predicate an
  indirect call's result does, so a value that cannot carry a loan ends
  the borrow at the copy while one that holds references or views keeps
  the protection (`ok-borrow-owned-element`,
  `exe-borrow-owned-element`).
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
- [x] A UTF-8 byte order mark at the start of a source is an `Invalid
  character` at 1:1: the lexer has no rule for the sequence, so an
  editor that saves one makes the whole file unreadable. Other compilers
  skip it; the spec says nothing, so this wants a decision before the
  lexer learns the three bytes.
  Resolved: the source view drops a leading `EF BB BF`, so the offsets
  every reader sees are the text's own and a file with the mark and one
  without are the same program; the grammar's lexical notes record the
  decision. The mark stays an invalid character anywhere else.
- [x] A struct kept two fields under one name and an enum two variants
  under one: nothing at the declaration checked, so a struct literal was
  asked for both fields and an access reached the first, while a second
  variant was unreachable. Both declarations reject a repeated name now,
  naming it, and the surviving member is the first
  (`err-duplicate-field`, `err-duplicate-variant`).
- [x] Qualified generic calls: `helper::id::<i32>(x)` answered
  "Unresolved value 'id'" because neither module-qualified resolution
  branch reached the generic-function lookup. Both do now, with the
  reachability check a qualified name owes (`ok-qualified-generic-call`,
  `err-qualified-generic-private`).
- [x] An associated call bound the owner's turbofish to the member: the
  two-segment branch passed the whole path's arguments on, so
  `Holder::<i32>::map_to(true)` pinned the member's first generic to
  `i32` and refused the argument, and an owner with two parameters
  reported a mismatch against the member's one written slot. A call's
  arguments bind the member's own parameters now, the way a method
  call's do, and the owner's turbofish stays the owner's
  (`ok-assoc-fn-infer` pins the numeric and the differently-typed
  call).
- [x] An associated function's turbofish names its own arguments: the
  parser records which segment the turbofish followed and the resolver
  reads it, so `P::id::<i32>(x)` instantiates the member while
  `P::<T>::id(x)` names the owner's (`ok-assoc-fn-turbofish`).
- [x] Read-before-write through an operator: an assignment reads its
  value before it takes the place, so `v[i] = v[i] + 1`,
  `m[k] = m[k] + 1`, and the compound forms compile without a
  temporary. A call that both reads and writes resolves the same way:
  the receiver is a reservation the call activates, which is the
  two-phase rule above.
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
- [ ] The cache is consulted after the frontend has parsed and
  resolved, so a hit cannot skip the work that dominates, and the
  compiler's version string does not identify the binary that wrote an
  entry: two builds of one dirty tree share every key.
- [ ] `alcy run` compiles without the cache, so a run pays the front
  end every time. The build's cache lookup and the run's lowering are
  two paths over the same work, and the run path is the one that has
  no key.

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
