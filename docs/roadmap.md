# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`. The language record lives in
`docs/spec/`; entries below cite the specification, not the history.

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
  instead is the refinement the ownership pass records. The `Drop` opcode is the
  marker a placed end carries, and the borrow checker consumes it as
  an authoritative boundary. Glue reaches a struct's or tuple's
  fields, an enum's live variant, and an array's elements; the unsafe
  intrinsic `drop_in_place<T>(p: &mut T)` lets `Vec` end its live
  elements in `clear` and `drop` (`Map` and `Set` already did by
  moving entries into bindings); and discarding - `_ := expr`, a bare
  statement, or a wildcard binding - ends the value where it stands.

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
  make its type move-only. `impl str` landed with
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
  one's own. Floats stay `PartialOrd` only.

- [x] Spec bounds and checked generics.

  The spec system's last slice: bounds where a parameter is declared - on
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
  bound. Designed in `docs/spec/grammar.md`'s function types, and
  the body-checking decision is recorded in `docs/spec/summaries.md`.

- [ ] Owned heap values.

  follow-up, and the recursive types an indirection
  unblocks. A function value is structurally typed, `Copy`, and
  carries no destructor glue, so whether a closure owns its captures
  cannot be a property of its type yet. The decision landed:
  a closure literal interns a nominal type whose field is
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
  frame-crossing iterators close the adapters' frame-bound caveat.
  Designed in `docs/spec/grammar.md`'s function types, with the owning
  function type and the capture regions recorded beside it.

- [ ] Package artifacts: a cache and resolution summaries.

  The binary IR form round-trips byte for byte; a `[lib]` is an opaque
  object because a dependent package cannot resolve against it. In
  slices: the build identity - source
  bytes, the manifest among them, pointer width, and toolchain, with a
  profile joining above the IR boundary the cache's design chose -
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
  and its checked body, and the dependent instantiates it, as
  `docs/spec/summaries.md` records.
  What remains is that work, and the spec implementations the roadmap
  asks a summary to carry. The design spans the IR format, the cache's
  key, artifact, location, and reporting, and the surface a dependent
  reads; the pieces are named in `docs/spec/deferred.md`.

- [x] The platform floor: a core-only `sys`, and the OS calls out of
  `core`.

  The floor landed: `sys` depends on `core` alone and owns `Fd` with the
  standard descriptors (associated functions), the faithful `read`/`write` that
  reports short and interrupted results as themselves, `OsError` over the raw
  errno, and the process arguments. The six declarations in `core/mem.al` moved
  out - the descriptor calls and the arguments to `sys`, the whole-file helpers
  to `io` - and `core`'s prelude re-exports retired. `sys_write` is one
  `write(2)` now and the write-all loop stayed with the print hooks; `sys_read`
  joined it, and the runtimes answer `-(errno)` (LLVM through the C library's
  error slot, freestanding through the kernel, wasm through WASI's
  `fd_read`/`fd_write`, whose playground host answers end of input). The landing
  also settled two surface rules: a selected member is a package root
  (`use sys::raw::write`), and an explicit `use` shadows a facade's name - the
  follow-up the suite's landing recorded. The exe runner's stdin field still
  waits on the input layer's cases.

- [ ] The standard library's surface is impls and specs.

  The parse family goes generic first, because input needs
  it: the `Parse` capability (a receiver-less call) is called
  as `i64::parse(text)` or `T::parse(text)`, and `String`'s six
  delegations collapse into one. The method spelling
  `str.parse::<T>()` waits on generic methods over builtin compound
  targets (its own slice). Primitives gain inherent impls (the
  checker's rule extends to `TypeKind::Primitive`, limited to the
  staged suite), and `num`'s per-type methods land with their first
  callers. The iterator adapters stay free functions until the spec
  system can express them as methods; that door is named, not
  promised.

- [x] Bytes-first input.
  Over `sys` and `Parse`: `io::BufReader` (one-call `fill`,
  `read_byte`, `read_line` with the CRLF rule and no UTF-8 validation,
  `read_to_end`; `EINTR` retried here and only here) and
  `io::Scanner` (ASCII whitespace, `next_token`, `next::<T>`, counted
  vector reads) with the three-way failure - end of input, a token
  that does not parse, the OS error underneath. wasm reads through
  WASI `fd_read` (end of input in the playground), and exe cases cover
  the path. proconio-class input stays out; its blockers are recorded
  in the specification.

- [x] The libc-free runtime, and `path` / `ffi` / `env` on it.

  The Linux hosted build links no C runtime, ever: the compiler emits
  `_start`, its naked trampoline reads argc and argv off the initial
  stack - and the environment pointer now rides as a third argument -
  and the process ends through the exit syscall, the same code a
  freestanding program was already running. A hosted build for
  Windows links `kernel32.dll` and nothing else: the entry gathers
  the arguments from `GetCommandLineW` with Rust's parse rules, the
  allocator is `VirtualAlloc`, and the file intrinsics open with
  `CreateFileW`. macOS keeps libSystem, the one sanctioned route
  there.

  The standard library grew `path` (one owned type with a `&Path`
  borrowed view, its UTF-8/UTF-16 internals selected by `#[when]`),
  `ffi` (`CString` for an owned, terminated string and `CStr` for a
  borrowed one), and `env` (`env`/`env_str` reading the platform's
  own table, `GetEnvironmentStringsW` on Windows, `getenv` on
  macOS, the inherited array on Linux); `io` now takes `&Path`
  throughout, and its intrinsics take `&[u8]`. The `err-io-file`
  corpus passes on both hosts - the Windows build runs under wine.

- [ ] `comp` expands what it knows, and the `fmt` pilot.

. Explicit `comp for`/`comp if` control flow whose remaining
  operations are emitted in the function's per-instantiation body,
  with the variadic tuple's shape readable at compile time (length,
  comp-known indexing that keeps each element's type) and
  variadic rule restated. The pilot rewrites `fmt`'s parse and its
  conversions in alcy (the builtins gain conversion impls; a float
  carries its precision), retires the C++ conversion lane once the
  tests, code size, compile time, runtime performance, wasm parity,
  and the float-digit accuracy item pass, and lets `print` ride the
  same expansion. Then removals follow: `builtin_display`
  retires, the fmt/io recognition moves under the mechanism, and the
  agreement test pinning the scalar comparisons to their impls lands
  early.

## 1.0

The number is a compatibility boundary rather than a feature count,
and the specification says what it means: the promises in `docs/spec/` stop
being editable there. It is declared when the boxes below are ticked,
and not on a date.

- [ ] Everything `docs/spec/` states is implemented, and
  `docs/spec/deferred.md` holds only what is explicitly moved past 1.0.
- [ ] Every decision this specification states is implemented or explicitly
      deferred, and none contradicts the code.
- [ ] The suite's public surface is described in `docs/spec/`, so the
  record of what is promised lives beside the record of why.
- [ ] A program written against the suite needs no `unsafe` outside
  the standard library's own internals.
