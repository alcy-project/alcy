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
- [ ] `map` has no constructor: its result type comes from a closure's
  return type, which inference cannot read before the closure is
  checked. A slice that checks a closure argument first, or that lets
  the adapter be spelled with a turbofish, gives it one.
- [x] A discarded temporary that owns memory ends where it stands:
  `_ := format("x")` and a bare `format("x")` are both ended by the
  discard path's `record_drop_in_place`, which the sanitized runs of
  `exe-discard-end` confirm. The `fmt` expansion still calls its
  collected tuple's glue directly, because the tuple is a scratch slot
  rather than a binding the pass could schedule.
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

<<<<<<< HEAD
- [x] Qualified generic calls: `helper::id::<i32>(x)` answered
  "Unresolved value 'id'" because neither module-qualified resolution
  branch reached the generic-function lookup. Both do now, with the
  reachability check a qualified name owes (`ok-qualified-generic-call`,
  `err-qualified-generic-private`).
- [ ] An associated function's turbofish reads as the owner's type
  arguments: `P::id::<i32>(x)` over a non-generic `P` is "Unexpected
  type arguments", and over a generic one the arguments bind the owner.
  The AST keeps one type-argument list per path, so the parser has to
  say which segment the turbofish followed.
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
||||||| parent of 03bb45ef (feat(analyzer): a deref of a call is a place)
- [ ] Assignment takes a syntactic place, so `*v.first_mut().unwrap()
  = 10` is refused ("Cannot assign to this place") where `*r = 10`
  works. Rust assigns through a deref of a call result; the checker
  would have to accept a deref as a place. Bind the reference for now.
- [ ] A method that takes `mut self: &mut Self` mutates a binding that
  is not `mut`: `s := String::new(); s.push(65u8)` compiles and runs.
  An explicit `&mut x` of a non-`mut` binding is admitted too; only a
  direct assignment (`*r = 7`) reports "Cannot assign to an immutable
  binding", so a method's write through its receiver is the gap.
  Rust wants the opposite: a write through a `&mut` the binding holds
  needs no `mut` (`let r = &mut x; *r = 1`), while `s.push` does. So
  `*fr = 10` is refused here unless `fr` is `mut`. The rule to settle
  is whether `mut` marks the binding or the access path.
- [ ] A borrow of a temporary that owns memory: `takes(&format("y"))`
  is refused because the temporary has no end site the drop pass can
  schedule, so materializing it would leak. It waits for the temporary's
  end, which the owning heap values work reaches. A method receiver is
  the same case: `format("x").to_ascii_lowercase()` leaks the `format`
  buffer, because the call borrows a temporary that no end site names.
=======
- [ ] A method that takes `mut self: &mut Self` mutates a binding that
  is not `mut`: `s := String::new(); s.push(65u8)` compiles and runs.
  An explicit `&mut x` of a non-`mut` binding is admitted too; only a
  direct assignment (`*r = 7`) reports "Cannot assign to an immutable
  binding", so a method's write through its receiver is the gap.
  Rust wants the opposite: a write through a `&mut` the binding holds
  needs no `mut` (`let r = &mut x; *r = 1`), while `s.push` does. So
  `*fr = 10` is refused here unless `fr` is `mut`. The rule to settle
  is whether `mut` marks the binding or the access path.
- [ ] A borrow of a temporary that owns memory: `takes(&format("y"))`
  is refused because the temporary has no end site the drop pass can
  schedule, so materializing it would leak. It waits for the temporary's
  end, which the owning heap values work reaches. A method receiver is
  the same case: `format("x").to_ascii_lowercase()` leaks the `format`
  buffer, because the call borrows a temporary that no end site names.
>>>>>>> 03bb45ef (feat(analyzer): a deref of a call is a place)
- [ ] Standard dependency selection: `--deps alcy/std/collections` on top
  of the default suite errors "Dependency 'alcy/std/*' overlaps
  'collections'". Make the default suite and the glob rule visible in
  `--help` and the diagnostic.
- [ ] `Map` key ergonomics: no entry API (`get_or_insert` first, an
  `Entry` once the borrow rules allow), and a missing key's panic prints
  the `Option::unwrap` wording. Generic keys wait for a `Hash` spec and
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

<<<<<<< HEAD
||||||| parent of 03bb45ef (feat(analyzer): a deref of a call is a place)
- [x] `Vec::first_mut` and `last_mut` answer the end elements as
  `Option<&mut T>`, so a build edits the ends in place.
- [x] `Vec::remove` answers the element at an index and keeps the rest
  in order, the ordered companion of `swap_remove`.
- [x] `Vec::fold` carries a state across the elements from the left,
  the reduce a sum or a build rides.
- [x] `Vec::all` and `any` answer the quantifiers over a predicate,
  with the empty vector vacuously true for `all` and false for `any`.
- [x] `Vec::find` answers the first element a predicate accepts as
  `Option<&T>`, the value form of `position`.
- [x] `Vec::position` answers the first index a predicate accepts as
  `Option<usize>`, so a search has no length check of its own.
- [x] `String::replace` swaps every non-overlapping occurrence of one
  view for another into a fresh string; an empty pattern copies.
- [x] `str::parse_u64` parses an unsigned decimal integer, taking a
  leading `+` and answering `None` on `-`, a non-number, or a `u64`
  overflow; `String` forwards it.
- [x] `str::parse_i64` parses a decimal integer with an optional sign
  and answers `None` on a non-number or an `i64` overflow; `String`
  forwards it.
- [x] `String::repeat` copies the bytes `n` times into a new string,
  the mirror of a view taken apart.
- [x] `Vec::reserve` grows the buffer for a known count without adding
  elements, so a build that knows its size allocates once.
- [x] `Vec::extend` moves another vector's elements onto the end and
  ends it, so a build can join two halves in place.
- [x] `Vec::insert` places a value at an index and shifts the rest up,
  appending when the index is the length.
- [x] `Vec::reverse` turns the elements around in place by swapping
  the ends inward.
- [x] `str::bytes` names the byte cursor `for b in s` already uses;
  `String` forwards it.
- [x] `[T]::first` and `last` answer the end elements as `Option<&T>`,
  matching `Vec` and `str`.
- [x] `Vec::retain` keeps the elements a predicate accepts in order
  and drops the rest, so a filter reads without a second vector.
- [x] `Vec::swap_remove` removes the element at an index and fills its
  place with the last one, the O(1) removal a set-like vector wants.
- [x] `String::pop` removes and answers the last byte as `Option<u8>`,
  the mirror of `push`.
- [x] `Vec::first` and `last` answer the ends as `Option<&T>`, so an
  empty vector is not a special case at the call site.
- [x] `str::split_at` answers the views before and after a byte
  offset; `String` forwards it.
- [x] `String::to_ascii_lowercase` and `to_ascii_uppercase` answer a
  new string with `A`-`Z` folded one way and every other byte copied.
- [x] `str::trim_start_matches` and `trim_end_matches` remove every
  leading or trailing occurrence of a pattern, leaving an empty one
  alone; `String` forwards both.
- [x] `String::truncate` keeps the first `n` bytes and ignores a
  longer length, so it cannot grow the buffer.
- [x] `str::is_ascii` reports whether every byte is below `0x80`;
  `String` forwards it.
- [x] `str::split_whitespace` is a cursor over the tokens between runs
  of ASCII whitespace, so no token is empty; `String` forwards it.
- [x] `str::lines` is a cursor over the `\n`-separated lines, with a
  trailing `\r` removed and no final empty line; `String` forwards it.
- [x] `str::count` answers the number of non-overlapping occurrences
  of a needle; `String` forwards it.
- [x] `str::strip_prefix` and `strip_suffix` answer the view without
  the prefix or suffix, or `None` when it is not there; `String`
  forwards both.
- [x] A float range in a `for` reports one diagnostic, "Float ranges
  do not iterate; a `for` loop needs an integer range", at the loop
  instead of errors inside `core/range.al`.
- [x] A `loop` whose body has no `break` diverges: the checker reads
  it as `!`, so a function ending in `loop { ... ret ... }` needs no
  trailing `ret`, and the lowering ends the dead exit with
  `unreachable`. A loop with a `break` is unchanged.
- [x] `str::rfind` answers the last byte offset of a needle; `String`
  forwards it.
- [x] `str::eq_ignore_ascii_case` compares bytes with only ASCII case
  folded; `String` forwards it.
- [x] `str::split_once` answers the two sides of the first delimiter,
  and `str::split` is a cursor over the pieces; `String` forwards
  both. The cursor takes its view by value, so it does not borrow a
  temporary.
- [x] A method call on a reference to a field resolves: `r := &s.a;
  r.len()` was refused because the reference carried the field's
  storage copy as its pointee while the method entry was keyed on the
  declared type. The receiver now looks through the pointee's origin
  too.
- [x] A reference to a block-local may not be the block's value: the
  lowering refuses `r := { x := 5; &x }`, which read the dead slot.
  The borrow pass's `ret &x` check covers a frame, not a binding.
- [x] `str::trim`, `trim_start`, and `trim_end` answer a sub-view with
  ASCII whitespace removed, and `String` forwards them.
- [x] A discard consumes its place. `_ := expr` and a bare statement
  ended an owning place in place without marking it moved, so a
  second discard or a use after a move double-freed. Both now emit
  the move marker first, and a later use is refused.
- [x] An out-of-range integer literal is an error. A suffix or an
  annotated position checks the magnitude against that type, and a
  negated literal may name the signed minimum. The value used to
  wrap silently (`256u8` became `0`).
- [x] A float's `!=` is the IEEE unordered comparison, so
  `nan != nan` is true. The LLVM backend used the ordered predicate,
  which is false for a NaN; the direct backend already did this.
- [x] `&&` and `||` short-circuit. Lowering emitted an eager bitwise
  operation, so the right side ran even when the left decided; it now
  branches and the right side runs only when the left does not decide.
- [x] `str::find` returns the first byte offset of a needle and
  `str::contains` reports whether one is there; `String` forwards both.
- [x] A missing key in `m[key]` panics with a map-specific message
  instead of the `Option::unwrap` wording.
- [x] A returned `Err` from `main() -> Result` is an exit status, not a
  panic: the message goes to stderr and the process exits 1, matching
  the direct backend.
- [x] Read-before-write through an operator: an assignment reads its
  value before it takes the place, so `v[i] = v[i] + 1`,
  `m[k] = m[k] + 1`, and the compound forms compile without a
  temporary. Two-phase borrows are still the general answer.
- [x] `Map::get_or_insert(key, value)` returns the entry as `&mut V`,
  inserting first when the key is absent; the value for a key already
  present is dropped.
- [x] A non-regular source reads to the end, so `--file /dev/stdin`
  consumes the pipe instead of reporting an empty file. The read is
  the fallback when there is no size to map.
- [x] A binary with no free `main` is diagnosed while checking and
  while building an executable, so the error names the missing entry
  instead of the linker. A library target is unaffected, and an IR or
  bitcode emission stays a module dump.
- [x] A manifest warns when a dependency requirement floats a pre-1.0
  minor (`0.x`): a 0.x minor may break, and the check lives in the
  parser without refusing the requirement (ADR-0057).
- [x] A format-string warning names a `{:.N}` precision without the
  explicit `f` conversion; `{:.2f}` is the spelling the diagnostics
  prefer, and both still compile.
- [x] The `map` adapter: a generic function binds a type slot from a
  function-typed argument, reading the closure's return type, so
  `map(v.iter(), (x: &i32) -> *x + 1)` spells neither type argument.
- [x] By-reference pattern bindings: `match *e` behind a shared or
  exclusive reference binds a payload or field as `&T`, so the
  ergonomic form works and `Option<T>`/`Result<T, E>` equality
  accepts a non-Copy `T`. Moving through the binding (`*s`) stays
  refused.
- [x] A match on a borrowed scrutinee: `match *self` reads the
  discriminant through the reference without loading the value, and
  a wildcard payload is neither loaded nor ended. `Option`/`Result`
  `is_some`/`is_none`/`is_ok`/`is_err` take `&Self`, so an option or
  result can be consulted again. Binding a non-Copy payload by value
  out of a borrow stays refused.
- [x] Structural equality for tuples and arrays: `==` and `!=` walk a
  tuple's fields or an array's elements when every element is a
  scalar or a thin address the backend compares directly, recursively
  for nested aggregates. A nominal or `str` element still waits for a
  per-element `eq` dispatch.
- [x] A borrowed Copy temporary: `a.eq(&2)`, `takes(&(1 + 2))`, and
  `take_p(&(P { x: 7 }))` materialize the operand into a slot and borrow
  that, instead of failing lowering.
- [x] Container destructors end their elements: `Vec::clear`/`Vec::drop`
  end the live elements through `drop_in_place`, and `Map::drop` binds
  each live entry so the drop pass ends it. A `Vec<String>`, a
  `Map<String>`, and a discarded `String` leave no LeakSanitizer
  block.
- [x] Discarded owning temporaries: a value bound to `_` that carries
  drop glue is ended like any other binding (`_ := format("x")`,
  `_ := String::with_capacity(64)`).
- [x] Qualified generic calls: a module-qualified path reaches the
  generic-function lookup (`helper::id::<i32>(x)`), and a turbofish
  after the member of an associated function (`P::id::<i32>`) names
  the member's arguments rather than the owner's, so the position of
  the turbofish decides which it is.
=======
- [x] Assignment takes a `&mut` a call answers: `*v.first_mut()
  .unwrap() = 10` is a place, because the deref types its inner as an
  expression when it is a call. A shared reference is still refused.
- [x] `Vec::first_mut` and `last_mut` answer the end elements as
  `Option<&mut T>`, so a build edits the ends in place.
- [x] `Vec::remove` answers the element at an index and keeps the rest
  in order, the ordered companion of `swap_remove`.
- [x] `Vec::fold` carries a state across the elements from the left,
  the reduce a sum or a build rides.
- [x] `Vec::all` and `any` answer the quantifiers over a predicate,
  with the empty vector vacuously true for `all` and false for `any`.
- [x] `Vec::find` answers the first element a predicate accepts as
  `Option<&T>`, the value form of `position`.
- [x] `Vec::position` answers the first index a predicate accepts as
  `Option<usize>`, so a search has no length check of its own.
- [x] `String::replace` swaps every non-overlapping occurrence of one
  view for another into a fresh string; an empty pattern copies.
- [x] `str::parse_u64` parses an unsigned decimal integer, taking a
  leading `+` and answering `None` on `-`, a non-number, or a `u64`
  overflow; `String` forwards it.
- [x] `str::parse_i64` parses a decimal integer with an optional sign
  and answers `None` on a non-number or an `i64` overflow; `String`
  forwards it.
- [x] `String::repeat` copies the bytes `n` times into a new string,
  the mirror of a view taken apart.
- [x] `Vec::reserve` grows the buffer for a known count without adding
  elements, so a build that knows its size allocates once.
- [x] `Vec::extend` moves another vector's elements onto the end and
  ends it, so a build can join two halves in place.
- [x] `Vec::insert` places a value at an index and shifts the rest up,
  appending when the index is the length.
- [x] `Vec::reverse` turns the elements around in place by swapping
  the ends inward.
- [x] `str::bytes` names the byte cursor `for b in s` already uses;
  `String` forwards it.
- [x] `[T]::first` and `last` answer the end elements as `Option<&T>`,
  matching `Vec` and `str`.
- [x] `Vec::retain` keeps the elements a predicate accepts in order
  and drops the rest, so a filter reads without a second vector.
- [x] `Vec::swap_remove` removes the element at an index and fills its
  place with the last one, the O(1) removal a set-like vector wants.
- [x] `String::pop` removes and answers the last byte as `Option<u8>`,
  the mirror of `push`.
- [x] `Vec::first` and `last` answer the ends as `Option<&T>`, so an
  empty vector is not a special case at the call site.
- [x] `str::split_at` answers the views before and after a byte
  offset; `String` forwards it.
- [x] `String::to_ascii_lowercase` and `to_ascii_uppercase` answer a
  new string with `A`-`Z` folded one way and every other byte copied.
- [x] `str::trim_start_matches` and `trim_end_matches` remove every
  leading or trailing occurrence of a pattern, leaving an empty one
  alone; `String` forwards both.
- [x] `String::truncate` keeps the first `n` bytes and ignores a
  longer length, so it cannot grow the buffer.
- [x] `str::is_ascii` reports whether every byte is below `0x80`;
  `String` forwards it.
- [x] `str::split_whitespace` is a cursor over the tokens between runs
  of ASCII whitespace, so no token is empty; `String` forwards it.
- [x] `str::lines` is a cursor over the `\n`-separated lines, with a
  trailing `\r` removed and no final empty line; `String` forwards it.
- [x] `str::count` answers the number of non-overlapping occurrences
  of a needle; `String` forwards it.
- [x] `str::strip_prefix` and `strip_suffix` answer the view without
  the prefix or suffix, or `None` when it is not there; `String`
  forwards both.
- [x] A float range in a `for` reports one diagnostic, "Float ranges
  do not iterate; a `for` loop needs an integer range", at the loop
  instead of errors inside `core/range.al`.
- [x] A `loop` whose body has no `break` diverges: the checker reads
  it as `!`, so a function ending in `loop { ... ret ... }` needs no
  trailing `ret`, and the lowering ends the dead exit with
  `unreachable`. A loop with a `break` is unchanged.
- [x] `str::rfind` answers the last byte offset of a needle; `String`
  forwards it.
- [x] `str::eq_ignore_ascii_case` compares bytes with only ASCII case
  folded; `String` forwards it.
- [x] `str::split_once` answers the two sides of the first delimiter,
  and `str::split` is a cursor over the pieces; `String` forwards
  both. The cursor takes its view by value, so it does not borrow a
  temporary.
- [x] A method call on a reference to a field resolves: `r := &s.a;
  r.len()` was refused because the reference carried the field's
  storage copy as its pointee while the method entry was keyed on the
  declared type. The receiver now looks through the pointee's origin
  too.
- [x] A reference to a block-local may not be the block's value: the
  lowering refuses `r := { x := 5; &x }`, which read the dead slot.
  The borrow pass's `ret &x` check covers a frame, not a binding.
- [x] `str::trim`, `trim_start`, and `trim_end` answer a sub-view with
  ASCII whitespace removed, and `String` forwards them.
- [x] A discard consumes its place. `_ := expr` and a bare statement
  ended an owning place in place without marking it moved, so a
  second discard or a use after a move double-freed. Both now emit
  the move marker first, and a later use is refused.
- [x] An out-of-range integer literal is an error. A suffix or an
  annotated position checks the magnitude against that type, and a
  negated literal may name the signed minimum. The value used to
  wrap silently (`256u8` became `0`).
- [x] A float's `!=` is the IEEE unordered comparison, so
  `nan != nan` is true. The LLVM backend used the ordered predicate,
  which is false for a NaN; the direct backend already did this.
- [x] `&&` and `||` short-circuit. Lowering emitted an eager bitwise
  operation, so the right side ran even when the left decided; it now
  branches and the right side runs only when the left does not decide.
- [x] `str::find` returns the first byte offset of a needle and
  `str::contains` reports whether one is there; `String` forwards both.
- [x] A missing key in `m[key]` panics with a map-specific message
  instead of the `Option::unwrap` wording.
- [x] A returned `Err` from `main() -> Result` is an exit status, not a
  panic: the message goes to stderr and the process exits 1, matching
  the direct backend.
- [x] Read-before-write through an operator: an assignment reads its
  value before it takes the place, so `v[i] = v[i] + 1`,
  `m[k] = m[k] + 1`, and the compound forms compile without a
  temporary. Two-phase borrows are still the general answer.
- [x] `Map::get_or_insert(key, value)` returns the entry as `&mut V`,
  inserting first when the key is absent; the value for a key already
  present is dropped.
- [x] A non-regular source reads to the end, so `--file /dev/stdin`
  consumes the pipe instead of reporting an empty file. The read is
  the fallback when there is no size to map.
- [x] A binary with no free `main` is diagnosed while checking and
  while building an executable, so the error names the missing entry
  instead of the linker. A library target is unaffected, and an IR or
  bitcode emission stays a module dump.
- [x] A manifest warns when a dependency requirement floats a pre-1.0
  minor (`0.x`): a 0.x minor may break, and the check lives in the
  parser without refusing the requirement (ADR-0057).
- [x] A format-string warning names a `{:.N}` precision without the
  explicit `f` conversion; `{:.2f}` is the spelling the diagnostics
  prefer, and both still compile.
- [x] The `map` adapter: a generic function binds a type slot from a
  function-typed argument, reading the closure's return type, so
  `map(v.iter(), (x: &i32) -> *x + 1)` spells neither type argument.
- [x] By-reference pattern bindings: `match *e` behind a shared or
  exclusive reference binds a payload or field as `&T`, so the
  ergonomic form works and `Option<T>`/`Result<T, E>` equality
  accepts a non-Copy `T`. Moving through the binding (`*s`) stays
  refused.
- [x] A match on a borrowed scrutinee: `match *self` reads the
  discriminant through the reference without loading the value, and
  a wildcard payload is neither loaded nor ended. `Option`/`Result`
  `is_some`/`is_none`/`is_ok`/`is_err` take `&Self`, so an option or
  result can be consulted again. Binding a non-Copy payload by value
  out of a borrow stays refused.
- [x] Structural equality for tuples and arrays: `==` and `!=` walk a
  tuple's fields or an array's elements when every element is a
  scalar or a thin address the backend compares directly, recursively
  for nested aggregates. A nominal or `str` element still waits for a
  per-element `eq` dispatch.
- [x] A borrowed Copy temporary: `a.eq(&2)`, `takes(&(1 + 2))`, and
  `take_p(&(P { x: 7 }))` materialize the operand into a slot and borrow
  that, instead of failing lowering.
- [x] Container destructors end their elements: `Vec::clear`/`Vec::drop`
  end the live elements through `drop_in_place`, and `Map::drop` binds
  each live entry so the drop pass ends it. A `Vec<String>`, a
  `Map<String>`, and a discarded `String` leave no LeakSanitizer
  block.
- [x] Discarded owning temporaries: a value bound to `_` that carries
  drop glue is ended like any other binding (`_ := format("x")`,
  `_ := String::with_capacity(64)`).
- [x] Qualified generic calls: a module-qualified path reaches the
  generic-function lookup (`helper::id::<i32>(x)`), and a turbofish
  after the member of an associated function (`P::id::<i32>`) names
  the member's arguments rather than the owner's, so the position of
  the turbofish decides which it is.
>>>>>>> 03bb45ef (feat(analyzer): a deref of a call is a place)
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
