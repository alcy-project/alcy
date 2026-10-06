# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`. The language record lives in
`docs/adr/`; entries below cite the decision, not the history.

## Next

- [x] Lib packages and suite manifests.

  The suite's other half next to `[[bin]]`: `[lib]` targets, path
  dependencies with cross-package `use` and export trimming, and
  compiler-side suite resolution, so a suite manifest means more
  than the embed-time member check. Summary-carrying artifacts
  stay with IR serialization; until then a lib artifact is an
  object file, linkable but opaque. This unblocks the package
  ecosystem, which the `unsafe` implementation waits for.
- [x] Function types and closures.

  `(params) -> body` with explicit `[captures]` modes (`name`,
  `&name`, `&mut name`), `ret` returning from the closure, and
  function types in type position; named functions coerce to
  values, and calls go through values. The owning environment —
  non-Copy captures and closures that escape their frame — is a
  follow-up in `deferred.md`.
- [x] C FFI and freestanding.

  In slices: the `unsafe` gate and the standard library's migration
  to it, raw pointers with casts, dereference, and offset,
  `extern "C"` declarations against the system libc, and the
  freestanding link mode: the compiler's `_start`, a per-declaration
  runtime reaching the kernel through raw syscalls, and an
  `mmap`-backed allocator, so `print` and the heap containers run
  with no crt, no loader, and no libc. Designed in
  `docs/adr/0050-unsafe-is-a-gate-on-operations.md`,
  `docs/adr/0051-extern-c-for-a-minimal-abi.md`, and
  `docs/adr/0052-freestanding-is-a-link-mode.md`.
- [ ] Operator specs: `Index`/`IndexMut` and `PartialEq`/`Eq`.

  Operators name specs the standard library owns: `a[i]` dispatches
  to `Index`, assignment through it to `IndexMut`, and `==` to
  `PartialEq`. The manifest gains what that needs - a `[spec]` table
  whose `suite-only` list names the specs only the declaring suite
  may implement, with an `implement` key reserved on the
  implementing side; and the spec system gains super-specs, so
  `Eq: PartialEq` says a reflexive type is comparable without
  repeating the method. The compiler keeps one canonical table from
  operator spelling to spec and method and verifies the standard
  library's declarations against it the way it verifies intrinsic
  shapes. In slices: the manifest seal and super-spec implication in
  the checker, then `Index`/`IndexMut` for `Vec`, `String`, and
  `Map`, then `PartialEq` and `Eq` for scalars with floats
  `PartialEq` only. Arithmetic and `Display` follow their own
  slices. Designed in
  `docs/adr/0053-operators-are-sealed-specs.md`.
- [ ] `comp` and `const` separate, with compile-time evaluation.

  `const` declares a compile-time value in item and local position;
  `comp` only says where code and parameters are evaluated, and
  `comp x := ...` retires. A `comp fn`'s calls are compile-time
  only - runtime arguments are an error and the result splices as a
  comp-known value - and no `const fn` exists. The evaluator
  becomes its own component with two readers: the checker, which
  needs `const` values for value parameters and array lengths, and
  lowering, which expands `fmt` and specializes calls. Today's AST
  engine lands first, then an IR interpreter replaces it behind the
  same interface, compared against the old engine over the corpus.
  Value parameters (`[T; N]`) land here, and `ArrayVec<T, N>` is
  their acceptance case rather than an item of its own. Designed in
  `docs/adr/0054-comp-and-const-are-separate.md`.
- [ ] Compile-time variadic arguments, and `print`/`println` in `io`.

  A parameter written `args: ..` takes the call's remaining
  arguments, which the compiler collects at compile time; no ABI
  changes, and only compiler-expanded functions may declare it at
  first. `print` and `println` move from `core` to `io`, which
  gains a `fmt` dependency and keeps one declaration for
  `println("hi")` and `println("x={}", x)`. The compiler's
  name-based fallback for them retires; `panic` stays in `core`. A
  hello world names `core`, `fmt`, and `io`, or the whole suite.
  Designed in
  `docs/adr/0055-compile-time-variadics-and-print-in-io.md`.
- [ ] Iterator and `Option` APIs.

  `Option` gains `map`, `and_then`, `unwrap_or`, `or_else`,
  `filter`, and `ok_or`; iterator adapters (`map`, `filter`,
  `fold`, `enumerate`, `take`, `skip`) follow. The first slice is
  frame-bound: an adapter holds closures whose captures stay in the
  frame that made it, and an iterator that crosses frames waits for
  the owning environment in `deferred.md`. The compiler slice the
  adapters need is a generic instantiation over a function type,
  which today cannot be mangled apart (ADR-0044); `collect` and
  generic constructors wait for spec bounds.
