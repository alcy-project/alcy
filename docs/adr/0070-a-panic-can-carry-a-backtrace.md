# ADR-0070: A panic can carry a backtrace

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-11

## Context

A panic names where it happened (`panic at <site>: <message>`), but not
how execution got there. A bounds failure three calls deep reads the
same as one in `main`, and the reader's only recourse is guessing which
call panicked. There is no unwinding in the language and none is
proposed; the question is only what the abort prints on its way out.

## Decision

`on-panic` is a profile key with two behaviors, both aborting:

```toml
[debug]
on-panic = "backtrace"
```

- `abort` (the default) writes the message and its site, as before.
- `backtrace` additionally walks the frame chain and prints one
  indented `at <name>` line per program frame it can name, innermost
  first, before aborting. A frame with a known call site appends it:
  `at f (main.al:6:3)`.

The mechanism is frame pointers plus two tables, LLVM only:

- The emitter records every defined function with its source spelling
  (`path::name`, without the signature the linkable name encodes) and
  defines `@alcy_backtrace_entries`: `{start, name, len}` per function
  plus a null terminator. The runtime's own frames and the entry thunk
  carry a null name, so the walk skips them.
- A named call gets a block of its own, and the emitter defines
  `@alcy_callsite_entries` the same way: one `{start, site, len}` per
  call, so a frame's return address resolves to the call it returns to
  rather than to a neighbor sharing its block. Successor PHIs are
  rewritten to the continuation, which the per-function verifier
  checks. Only a backtrace build splits; every other build emits calls
  inline and the table stays empty.
- Every generated function keeps a frame pointer, and tail calls are
  off, so the panic's frame stays a frame of its own. `alcy_panic`
  itself is `noinline`: inlining it into the caller would start the
  walk one frame too deep. The user entry is `noinline` too: the
  optimizer would otherwise fold it into its C-ABI wrapper and leave
  the walk no named frame at all, and one missed inlining on a function
  called once costs nothing.
- The walk starts at the panic's frame and follows saved frame pointers
  outward, matching each return address against the entry with the
  greatest start below it and within range; anything farther is another
  mapping (libc, the sanitizer runtime) and not a match. The comparison
  is strict because a call that ends its function returns to the address
  the next function begins at, and that frame belongs to the function
  that made the call; an unpadded layout puts those two addresses on top
  of each other. It stops after 64 frames, on a null or non-advancing
  frame, and prints nothing when nothing matches rather than guessing.
  Each frame looks up both tables: the function name, then the site
  after it.
- The walk reads the x86-64 SysV and AArch64 frame layout (`[fp]` is
  the caller's frame, `[fp+8]` its return address; Apple spells AArch64
  `arm64`). On any other target the panic carries its message and site
  only, even with backtrace on: Windows x64's frame pointer is an
  RSP-relative point in the fixed frame by design, which is not a chain
  to walk.
- An optimized build inlines and tail-calls less visibly: `release`
  with backtrace shows the frames that survived, which can be just
  `main`. That is best effort, not a second unwinder, and documented as
  such. A call site survives with its block: an address-taken block is
  not merged away, so the surviving frame still resolves to the call
  it returns to.

Resolution follows the profile order (ADR-0069): built-in `abort`,
the selected table's `on-panic`, then `--on-panic`, which every verb
carries. `compile` reads no profile file, so a custom profile's key is
unreachable there and `--on-panic` is the whole override.

## Consequences

- A debugging panic reads as a cause and a path: the site says which
  check fired, the frames say which call reached it.
- The default artifact is byte-identical to before: no table, no frame
  attributes, no walker, no extra inlining boundary. The backtrace
  build pays frame pointers, no tail calls, and two `noinline`s, which
  is why it is opt-in rather than always on.
- The direct wasm backend does not walk: it writes the message and its
  site and exits. A wasm backtrace wants a shadow stack over every
  call, which is a separate design.
- The table's names are source spellings, so a backtrace reads
  `pkg/mod::fn`. ADR-0011's "backtraces show encoded names" is
  superseded for panics: the encoder stays unreadable by design, and
  the table carries the readable form instead.
- `main returned Err` and allocation failures walk like any other
  panic but carry no site; their frames still print.

## Alternatives considered

- **Always on.** Rejected: frame pointers, disabled tail calls, and
  the entry boundary change every artifact's codegen for a report most
  runs never print.
- **DWARF line tables instead of a table.** Rejected: the compiler
  emits no debug info today, and parsing it at panic time needs a
  reader the runtime does not have. The table is a dozen words per
  function and read with two loads.
- **No-inline everywhere under backtrace.** Rejected: full fidelity at
  the cost of the optimizer's main tool, silently, under a flag that
  promises optimized code. One boundary (the entry) is kept; the rest
  is best effort.
- **Shadow stack for all backends.** Deferred: it instruments every
  call on every backend including wasm. The frame walk covers the
  native crashes that need reading first.

## Reference

- `src/codegen_llvm/llvm_ir_emitter.{h,cc}`: `BacktraceName`,
  `define_backtrace_table`, the entry `noinline`, and the table slot.
- `src/codegen_llvm/runtime_ir.{h,cc}`: `build_panic_walk` and the
  `noinline` on `alcy_panic`.
- `src/codegen_llvm/emit.cc`: table definition and the frame
  attributes, before optimization.
- `src/codegen/backend.h`: `EmitRequest.backtrace`, the only
  profile value a backend sees.
- `docs/adr/0011-symbol-mangling.md`: encoded names stay; the table
  carries the readable ones.
- `docs/adr/0069-build-settings-live-in-named-profiles.md`: where
  `on-panic` resolves.
