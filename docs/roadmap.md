# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`. The language record lives in
`docs/adr/`; entries below cite the decision, not the history.

## Next

- [ ] Lib packages and suite manifests.

  The suite's other half next to `[[bin]]`: `[lib]` targets with
  summary-carrying artifacts and cross-package compilation, and
  compiler-side suite resolution so a suite manifest means more than
  the embed-time member check. Path dependencies already load from
  source with cross-package `use` and export trimming; what remains
  is the suite resolution this unblocks the package ecosystem with,
  which the `unsafe` implementation waits for.
- [ ] Function types and closures.

  Design first, then build: the syntax against `||`, what a closure
  captures and how, and how the environment lowers — then the
  implementation. This completes the iteration story the range work
  started.
- [ ] C FFI and freestanding.

  In slices: the `unsafe` design, then `extern "C"` declarations
  against the system libc, then `_start`, raw syscalls, and an
  allocator without libc. Needs the driver work for nostdlib-style
  links; the libc fight must not gate the FFI value.
- [ ] `ArrayVec<T, N>`, a fixed-capacity inline container.

  Blocked on value parameters: array lengths take decimal literals
  only, so a capacity parameter cannot be named in the language as
  it stands. When value parameters land, this is the baremetal case
  proper: no heap, so no realloc hazard, and the growth logic it
  shares with `Vec`.
