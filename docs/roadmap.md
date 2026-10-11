# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`. The language record lives in
`docs/adr/`; entries below cite the decision, not the history.

## Next

- [ ] Ownership and drops per CFG path.

  A loan's extent is a region, solved per path; ownership is not.
  Lowering tracks a moved flag per local with a one-bit latch, so a
  value consumed on one arm of an `if` and not the other reports "use
  of moved" - the in-tree case is `Option::filter` over a non-Copy
  `T` - a store that re-owns a place leaks the new value, and a
  destructor that needs a variant's discriminant or an array's
  elements cannot be placed: a droppable local or field warns, a
  wildcard match discard leaks its payload silently, and `Vec`,
  `Map`, and `Set` release a buffer without ending the live elements
  in it. In slices: lowering journals an end site per place and stops
  placing ends; a `drop` pass beside lowering reads the `Move`
  markers and the stores, solves ownership per place (owned, moved,
  maybe), appends an end where every path owns, splits the edges
  where the paths differ, and keeps a per-place flag only where a
  loop carries both states; the `Drop` opcode becomes the marker a
  placed end carries, and the borrow checker consumes it as an
  authoritative boundary, which retires the remaining false
  positives; then drop glue for enum payloads (a discriminant
  switch) and array elements (an element loop), ending a wildcard
  discard's payload and giving the `drop_in_place` the owning
  pointers of the fifth item need; then the containers end live
  elements in `clear` and `drop`; and one rule replaces today's
  `_ := expr` error beside a bare statement's warning, both ending
  the value. Designed in
  `docs/adr/0062-a-values-end-follows-the-path-that-owns-it.md`.

- [ ] Container iteration.

  The adapters landed, and no standard container can be walked: `Vec`,
  `Map`, and `Set` have no cursor, so `for x in v` is "No method
  'into_iter'" and std algorithms index in loops. In slices: `Vec`
  cursors in the item-parameter shape the adapters already use
  (`VecIter<T, Item>`), with the identity `into_iter` the `for`
  desugar needs; `Map`'s `keys`, `values`, and `iter` over `str` and
  `&V`, and `Set` behind them; the free algorithms the cursors make
  writable (`count`, `any`, `all`, `position`, and `find`/`last`
  with the item type spelled); and one compiler slice extends
  inherent impl targets beyond nominals to `[T]`, `[T; N]`, and
  `str`, which ADR-0028 leaves for a later extension, so arrays,
  slices, and strings get their own methods (`.len()`, `.iter()`,
  `into_iter`) at all. `collect` in its bound-shaped form waits for
  the fourth item; a `collect_into(it, &mut Vec<T>)` free function
  does not. Designed in `docs/adr/0028-spec-system.md`.

- [ ] Operator specs: `Display`, floats, arithmetic, and ordering.

  The first slice sealed `Index`/`IndexMut` and `PartialEq`/`Eq`;
  ADR-0053 left the rest for their own slices. In order: floats join
  the convertible types (integers, `bool`, and `str` today) so
  `println("{}", 3.5)` compiles; `Display` for `{}` needs a signature
  a user type can implement, a decision on whether the two-pass
  measure/expand expansion may run a user method twice, and a
  decision on whether the spec is sealed; then arithmetic (`Add`,
  `Sub`, `Mul`, `Div`, `Rem`, `Neg`) with canonical operator shapes
  and `x op= y` routed through them; then `Ordering`, `PartialOrd`,
  and `Ord` with comparison dispatch, which is what `Vec::sort` waits
  for. Floats stay `PartialOrd` only. The implementing side of a seal
  (`[spec] implement`) ships when the first sealed spec a user
  package must implement needs it. Designed in
  `docs/adr/0053-operators-are-sealed-specs.md`.

- [ ] Spec bounds and checked generics.

  ADR-0028's last slice: bounds where a parameter is declared - on
  functions, methods, spec methods, and impls, never on structs or
  enums - multiple bounds joined with `+`, `where` still reserved,
  each call checked against them. Today a generic body is checked
  only when instantiated, so an incomparable element's error appears
  inside `collections/vec.al` rather than at the user's `==`, and an
  uninstantiated body is never checked at all. In slices: bounds in
  the parameter list, where the annotation today means a value
  parameter; a check of each instantiation's type arguments, with
  super-spec implication; a check of a generic impl's bounds before
  its body specializes, so the operator case names the user's type;
  bounds across `core` and `collections`, which gives `collect` its
  shape and needs a decision on inferring an item type through an
  `Iterator<T>` bound; spec methods with type parameters; and one ADR
  deciding definition-site body checking (which needs a symbolic
  type parameter the IR does not have) against call-site-only
  checking. Callable bounds follow once the vocabulary exists; an
  inferred argument that is a function type waits for them. Designed
  in `docs/adr/0028-spec-system.md`.

- [ ] Owned heap values.

  ADR-0044's follow-up, and the recursive types an indirection
  unblocks. A function value is structurally typed, `Copy`, and
  carries no destructor glue, so whether a closure owns its captures
  cannot be a property of its type yet; the first slice is that
  decision, in an ADR (move-only function values, nominal per-literal
  closure types, or a value carrying an environment-drop entry).
  Then `Box<T>` over the allocator's `MaybeUninit<T>` - a
  hand-written box compiles today - whose destructor ends its pointee
  with the first item's `drop_in_place`, with a recursive enum or
  struct built through it as the acceptance case. Then the owning
  environment: non-Copy captures move in, the environment is
  allocated where the closure value can own and drop it, the `Copy`
  restriction and the frame-escape refusal retire, and frame-crossing
  iterators close the adapters' frame-bound caveat. Designed in
  `docs/adr/0044-function-types-and-closures.md`.

- [ ] Package artifacts: a cache and resolution summaries.

  The binary IR form round-trips byte for byte and has no production
  reader; a `[lib]` is an opaque object because a dependent package
  cannot resolve against it. In slices: the build identity (source
  bytes, manifest, flags, and toolchain) and the content hashes the
  format's `FILES` section reserves but the writer leaves empty; a
  cache of the checked artifact, its location and invalidation, and a
  hit or miss in the command envelope; a versioned summary of what a
  dependent needs - signatures, nominal shapes, generic instances,
  and spec implementations - that the analyzer consumes in place of
  the dependency's source; multi-object linking, which the runtime's
  external linkage needs; and the fetchers that make a registry or
  git dependency obtainable at all. The cache without summaries and
  the summaries without fetchers each go half the way, so the item
  states both. Designed in
  `docs/adr/0056-the-ir-has-a-text-and-a-binary-form.md`; the pieces
  are named in `docs/spec/deferred.md`.
