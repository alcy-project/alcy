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
- [ ] `ArrayVec<T, N>`, a fixed-capacity inline container.

  Blocked on value parameters: array lengths take decimal literals
  only, so a capacity parameter cannot be named in the language as
  it stands. When value parameters land, this is the baremetal case
  proper: no heap, so no realloc hazard, and the growth logic it
  shares with `Vec`.
