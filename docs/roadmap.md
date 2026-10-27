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

- [x] Container iteration.

  The adapters landed, and the standard containers now have cursors:
  `Vec` consumes (`VecIntoIter`), borrows (`VecIter`), and borrows
  exclusively (`VecIterMut`); `Map` and `Set` hand out entries, keys,
  and values from shared cursors, and `Map` consumes (`MapIntoIter`).
  `for x in v` and `for (k, v) in m` walk them, a break ends what the
  loop never took, and the free algorithms (`count`, `any`, `all`,
  `position`, `find`, `last`, `collect_into`) ride any iterator -
  `collect_into` is the shape `collect` waits on bounds for. A cursor
  that yields a reference needed the checker to accept a spec argument
  naming one of the implementation's parameters (`Iterator<&T>`); a
  cursor whose element may be Copy needed a declared destructor to
  make its type move-only (ADR-0013, amended). `impl str` landed with
  `len`, `is_empty`, `byte`, and byte iteration; inherent impls may
  target compound shapes (`impl<T> [T]`, `impl<T, N: usize> [T; N]`),
  so slices and arrays answer `len`, `is_empty`, `as_slice`,
  `as_mut_slice`, `iter`, `iter_mut`, and `into_iter`. The index loops
  that remain either pair two sequences (the vector's own `==`) or
  walk a slot table (the map's sweeps), which the cursors do not
  replace.

- [x] Operator specs: floats, `Display`, arithmetic, and ordering.

  The first slice sealed `Index`/`IndexMut` and `PartialEq`/`Eq`;
  floats and arithmetic landed after it. Floats joined the
  convertible types: `{}` prints a decimal magnitude with up to six
  fractional digits, rounded half-up and trimmed, and a mantissa with
  an exponent outside [1e-4, 1e16); `{:.Nf}` fixes the fractional
  digits and `{:.N}` is the same piece. Arithmetic landed as `Add`,
  `Sub`, `Mul`, `Div`, `Rem`, and `Neg` in `core` with the canonical
  by-value shape, `x op= y` stored back through the same
  implementation, and the implementing side (`[spec] implement`)
  reaching user packages. Ordering landed too: `Ordering`,
  `PartialOrd` (`Option<Ordering>`, so an incomparable pair makes all
  four comparisons false), `Ord`, and the dispatch for `<`, `<=`, `>`,
  and `>=`, with `Vec::sort` riding `Ord` and `sort_by` taking the
  order as a predicate. `Display` landed last: a nominal argument
  formats through `fmt`'s `fn display(self: &Self, out: &mut [u8]) ->
  usize`, which writes the prefix that fits and returns the total the
  measure pass learns from an empty slice, so `{}` reaches a type of
  one's own. Floats stay `PartialOrd` only. Designed in
  `docs/adr/0053-operators-are-sealed-specs.md`.

- [x] Spec bounds and checked generics.

  ADR-0028's last slice: bounds where a parameter is declared - on
  functions, methods, spec methods, and impls, never on structs or
  enums - multiple bounds joined with `+`, `where` still reserved,
  each call checked against them. The first slices landed: `T: A + B`
  parses and a declaration validates that each annotation names a
  spec (a single primitive integer is a value parameter, as before);
  a call checks each instantiation's type arguments, with super-spec
  implication; and a generic impl's bounds are checked before its body
  specializes, so `Vec<Plain> == Vec<Plain>` names `Plain` at the
  user's `==` rather than `vec.al` - the standard library's generic
  `PartialEq`/`Eq` implementations carry their bounds now. A concrete
  impl's spec method may declare type parameters of its own: the
  declaration and the implementation compare under one anonymous
  stand-in per parameter, so the names need not match, and a call that
  spells them resolves one instantiation through the generic method
  registry. A generic impl carries a method's own parameters too: the
  declaration checks the impl's arity, and the call instantiates the
  member against it. `collect`
  landed with bound-driven inference: a parameter no argument binds
  (`I: Iterator<T>`'s item type) comes from the first declared
  method's signature and the implementation for the bound-covered
  parameter, so `collect(v.iter())` spells nothing. The tail closed
  without the callable bound the item predicted: a function-typed
  argument instantiates like any other, because the body is checked at
  the call and the parameter is already the concrete function type
  there, so `id((a: i32) -> a)` and calling through `f(x)` need no
  bound. Designed in
  `docs/adr/0028-spec-system.md`, and the body-checking decision is
  `docs/adr/0063-generic-bodies-are-checked-at-their-calls.md`.

- [ ] Owned heap values.

  ADR-0044's follow-up, and the recursive types an indirection
  unblocks. A function value is structurally typed, `Copy`, and
  carries no destructor glue, so whether a closure owns its captures
  cannot be a property of its type yet. The decision landed
  (ADR-0064): a closure literal interns a nominal type whose field is
  its environment tuple, so its `Copy`-ness is the environment's and a
  closure coerces to a bare function type as a borrow of it. The first
  half landed with `Box<T>`: a literal's type is that struct, a value
  coerces to a bare function type as a view that borrows it, and the
  drop walk ends the value through the field - so a non-`Copy` capture
  moves in, is owned like any other field, and moving one out of the
  body is refused as moving out of borrowed content. `Box<T>` landed
  over the allocator's `MaybeUninit<T>`: `new` moves the value in,
  `get`/`get_mut` borrow it, `into_inner` moves the pointee out, and
  the destructor ends the pointee with the first item's
  `drop_in_place` and releases the block. A `live` flag is what a
  spent handle says, so a recursive enum's consuming walk frees every
  link exactly once - the sanitizer run checks it. What remains is the
  owning environment where the view must own it: the environment is
  allocated on the heap, the frame-escape refusal retires, and
  frame-crossing iterators close the adapters' frame-bound caveat. Designed in
  `docs/adr/0044-function-types-and-closures.md`, with the owning
  function type in `docs/adr/0075-owning-function-types.md` and capture
  regions in `docs/adr/0076-region-annotated-captures.md`.

- [ ] Package artifacts: a cache and resolution summaries.

  The binary IR form round-trips byte for byte; a `[lib]` is an opaque
  object because a dependent package cannot resolve against it. In
  slices: the build identity - source
  bytes, the manifest among them, pointer width, and toolchain, with a
  profile joining above the IR boundary where ADR-0067 keeps the cache -
  and the content hashes the format's `FILES` section reserves but the
  writer leaves empty; a cache of the checked artifact, its location
  and invalidation, and a hit or miss in the command envelope; a
  versioned summary of what a dependent needs - signatures, nominal
  shapes, generic instances, and spec implementations - that the
  analyzer consumes in place of the dependency's source; multi-object
  linking, which the runtime's external linkage needs; and the fetchers
  that make a registry or git dependency obtainable at all. The cache
  without summaries and the summaries without fetchers each go half the
  way, so the item states both. Four slices landed: each `FILES` record
  carries the content hash of the source it names (XXH3-128 of its
  bytes), so a cache can tell two programs under one name apart; the
  `IDENTITY` section carries one digest over the file table, the pointer
  width, and the toolchain (`ir::identity_digest`), so a cache compares
  one number instead of walking the tables; the cache itself holds
  the unoptimized binary IR under one digest over the identity, the
  manifest, and the target, reporting `hit`, `miss`, or `off` in the
  envelope; and a `[lib]` target's IR binary carries a `SUMMARY` of the
  package's public surface - the code that carries each signature, and
  a generic declaration marked as needing its body - which the reader
  rebuilds and the cache hit path hands back. A dependent now resolves
  against a summary in place of the dependency's source, and the
  generic half is decided: a generic item crosses as its declaration
  and its checked body, and the dependent instantiates it
  (`docs/adr/0079-a-generic-item-crosses-as-its-checked-body.md`).
  What remains is that work, and the spec implementations the roadmap
  asks a summary to carry. Designed in
  `docs/adr/0056-the-ir-has-a-text-and-a-binary-form.md`, for the
  cache's key, artifact, location, and reporting,
  `docs/adr/0072-a-packages-cache-holds-its-binary-ir.md`, and for the
  surface a dependent reads,
  `docs/adr/0078-the-summary-a-dependent-reads.md`; the pieces
  are named in `docs/spec/deferred.md`.

## 1.0

The number is a compatibility boundary rather than a feature count,
and ADR-0099 says what it means: the promises in `docs/adr/` stop
being editable there. It is declared when the boxes below are ticked,
and not on a date.

- [ ] The spec's MVP sections are implemented, and every staged or
  deferred marker is implemented or moved to an explicit post-1.0
  list in `docs/spec/deferred.md`.
- [ ] Every Accepted ADR is implemented or explicitly deferred, and
  none contradicts the code.
- [ ] The suite's public surface is described in `docs/spec/`, so the
  record of what is promised lives beside the record of why.
- [ ] A program written against the suite needs no `unsafe` outside
  the standard library's own internals (ADR-0100).
