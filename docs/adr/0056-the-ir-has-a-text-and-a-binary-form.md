# ADR-0056: The IR has a text and a binary form

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-07

## Context

The IR is dense index tables and has no form outside the process: the
only thing a build can write is one backend's output, and LLVM IR is
that backend's, not alcy's. Two needs are already recorded and both
wait on a form:

- `emit_mode.h` reserved `ir` for "alcy's own intermediate
  representation, which earns the spelling once it can be written": a
  compiler developer debugging a lowering change wants to read what
  the pipeline produced, next to the LLVM IR they can read today.
- The roadmap leaves summary-carrying artifacts to IR serialization,
  and ADR-0054 asks for an IR interpreter as comp evaluation's second
  engine. Both want a machine form that is self-describing, versioned,
  and cheap to check, not a text to parse.

The constraints are the project's usual ones: `-fno-exceptions`, no
second source of truth that can drift, and output that does not depend
on the host or the job count (ADR-0048). The IR is also the pipeline's
one backend-neutral currency, so a form for it must not be bound to
LLVM or to any one target.

## Decision

**Two forms, two jobs.** `--emit=ir` writes a text view for people, and
`--emit=ir-bc` writes the binary form for machines. Both serialize the
lowered package: the storage, the string interner the storage's ids
resolve in, the instruction spans with a file table, the address names,
and the prelude count. Both are backend-free: they
run before a backend is chosen, so a build with no LLVM, no linker, or
no target named writes them, and `--backend`/`--target` are ignored.
`--release` is refused with a diagnostic, because neither form is
optimized and saying so is cheaper than being asked why later. Output
defaults are `.ir` and `.irb`.

**The text form is a view, not a serialization.** It is write-only: no
parser is promised, and text-to-IR is not a supported operation, so
the writer prints what is readable rather than what is re-readable.
Its shape follows alcy: declarations (`struct Name(T, T)`,
`enum Name { V(T), W }`, `extern fn name(...)`), alcy type spellings
(`i32`, `bool`, `str`, `&T`, `&mut T`, `*T`, `*mut T`, `[T]`, `[T; N]`,
`(A, B)`, `fn(A) -> R`, `(A) -> R`), inline literals (`5`, `"hi"`,
`true`), and alcy's operator spellings for everything the operands'
types determine uniquely (`+`, `/`, `%`, `<<`, `>>`, `==`, `as`, `!`
on `bool`, `~` elsewhere). Values keep their storage indices (`v12`,
`b7`) so a verifier report and the text name the same thing. IR-only
operations keep keywords: `alloca`, `load`/`store` as `*p`/`*p = v`,
`addr(...)`, `insert`, `select`, `move`, `drop`, borrow as `&place`,
`br`/`condbr`/`switch`/`ret`/`unreachable`, and the atomic and size
queries. Each instruction ends with its span as a comment
(`// file:offset+length`) when spans are available.

**The binary form is the canonical one.** Magic `ALIR`, a major/minor
version, the pointer width, the compiler's version string, and a
section table whose entries are `(kind, offset, size)`: a reader skips
a section it does not know, and a major bump is the only way to change
what an existing section means. Tables are fixed-width records in
index order, strings live in one blob, and every `StringPoolId` is
written as an index into it, so a reader can re-intern the strings and
remap the references. A `FILES` section names each file
(`name`, `size`, content hash) and `SPANS` holds one record per
instruction, so a cached package can still report spanned diagnostics.
The footer carries an integrity hash. Writing the same IR twice
produces the same bytes: no pointer values, no hash-map order, no
host-dependent widths.

**The binary form is verified on load.** `deserialize` rebuilds a
`StorageState`, re-interns the strings, remaps the references, and
goes through `StorageBuilder::build()` so the only thing a reader can
return is a `VerifiedStorage`; truncation, an unknown major version,
an out-of-range index, or overlapping sections is a structured error.

**One spelling table.** The text mnemonics live beside the opcode names
(`ir::opcode_mnemonic`), and a test enumerates every `Opcode` so a new
instruction cannot ship without a spelling. The binary keeps the tables
themselves; nothing derives one form from the other.

## Consequences

Reading lowered IR no longer requires LLVM, which is what a
no-LLVM build needed to inspect its own output, and the two forms are
the groundwork for the cache and the interpreter: both consume the
binary, and neither has to know the frontend.

The costs are a second writer to keep current with the IR, a versioning
discipline for the binary, and a text form that is deliberately not a
contract for parsing. Out of scope, each with its own decision: the
build cache itself (key, location, invalidation, hit reporting), the
IR interpreter, and cross-major-version readers.
