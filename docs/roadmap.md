# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`. The language record lives in
`docs/adr/`; entries below cite the decision, not the history.

## Next

- [x] Ownership and drops per CFG path.

  Lowering placed ends with a flag per local and a one-bit latch, so a
  value consumed on one arm of an `if` and not the other reported "use
  of moved" (`Option::filter` over a non-Copy `T`), a store that
  re-owned a place leaked the new value, and a destructor that needed
  a variant's discriminant or an array's elements could not be placed
  at all. Lowering now journals an end site per place and the
  ownership events, and a `drop` pass beside it reads the `Move`
  markers and the stores, solves ownership per place (owned, moved,
  maybe), and appends an end where every path owns; paths that differ
  decide at run time behind a per-place flag, and splitting the edge
  instead is the refinement ADR-0062 records. The `Drop` opcode is the
  marker a placed end carries, and the borrow checker consumes it as
  an authoritative boundary. Glue reaches a struct's or tuple's
  fields, an enum's live variant, and an array's elements; the unsafe
  intrinsic `drop_in_place<T>(p: &mut T)` lets `Vec` end its live
  elements in `clear` and `drop` (`Map` and `Set` already did by
  moving entries into bindings); and discarding - `_ := expr`, a bare
  statement, or a wildcard binding - ends the value where it stands.
  Designed in
  `docs/adr/0062-a-values-end-follows-the-path-that-owns-it.md`.

- [ ] Container iteration.

  The adapters landed, and the standard containers now have cursors:
  `Vec` consumes (`VecIntoIter`), borrows (`VecIter`), and borrows
  exclusively (`VecIterMut`); `Map` and `Set` hand out entries, keys,
  and values from shared cursors, and `Map` consumes (`MapIntoIter`).
  `for x in v` and `for (k, v) in m` walk them, a break ends what the
  loop never took, and the free algorithms (`count`, `any`, `all`,
  `position`, `find`, `last`, `collect_into`) ride any iterator -
  `collect_into` is the shape `collect` waits on bounds for. A cursor
  that yields a reference needed the checker to accept a spec argument
  naming one of the implementation's parameters (`Iterator<&T>`),
  which it now does. `impl str` landed too: the string view has
  `len`, `is_empty`, `byte`, and byte iteration after it, so `for b in
  s` reads bytes and `s.len()` replaces the free intrinsic call. And
  inherent impls may target compound shapes: `impl<T> [T]` and
  `impl<T, N: usize> [T; N]` register through a shape the checker
  matches against a receiver, so slices and arrays answer `len`,
  `is_empty`, `as_slice`, and `iter`, and `for x in s.iter()` walks
  either one by reference through `slice_at`. What remains: consuming
  an array (`for x in a`, as opposed to `for x in a.iter()`), which
  needs a way to read an element out, and the std cleanup that
  replaces index loops with the algorithms.

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
