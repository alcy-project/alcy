# ADR-0011: Every Symbol Is Derived from Its Signature

- Status: Accepted
- Date: 2026-09-26

## Context

Emitted symbols were the source names. `alcy_runtime.c` defines
`alcy_dealloc`, which calls `free`; a container method named `free`
compiled to the ELF symbol `free`. The two collided, `alcy_dealloc`
called the alcy method, and the pair recursed into each other: double
frees, wild pointers, a crash on a program that only pushes an integer.

Nothing about that program was unusual. `free`, `write`, `read`,
`open`, `close`, `exit`, `stat`, and `fork` are the obvious names for
the syscall wrappers a systems language exists to write, and every one
of them is a C library export. Requiring users to avoid them is not a
policy a low-level language can adopt.

Two properties were also missing. A name had to be stable across
builds, because a symbol is what a profiler, a debugger, and a
backtrace read. And it had to distinguish the things the compiler
actually emits more than one of: the same method on two instantiations,
or an associated function and a method that share a name.

## Decision

A symbol is derived from the signature it names, never from the source
name. The signature is the module path, the item name, what the item
is, and the type arguments of the instantiation. `src/symbol` encodes
it to a string beginning `_A`, and decodes it back.

```
symbol   = "_A" version kind seglist name generics
kind     = "f" free | "a" associated | "m" method
seglist  = { segment } "."
segment  = DIGITS name
name     = DIGITS name
generics = { type }
type     = prim | "z" str | "p" ptr
         | "r" type | "w" type | "y" DIGITS type
         | "u" DIGITS { type } | "n" seglist DIGITS { type }
```

Three decisions inside that shape are worth stating.

**A length, not a separator, delimits every name.** A segment and an
item name carry their byte length, so any byte may appear in a name,
`::` and `.` included, and nothing needs escaping.

**A segment list is terminated, a type list is not.** A type begins
with a letter and a length begins with a digit, so a list of types ends
where a digit or the end of the symbol appears. A list of segments
cannot: the item that follows it is itself length-prefixed and would be
read as one more segment, so it carries a `.`.

**A tuple and a nominal's arguments carry a count.** Their extent
cannot depend on what follows, because a following type is
indistinguishable from another element.

`version` is a single character, so a decoder rejects an encoding it
does not understand instead of misreading it. `display` renders a
decoded signature in source spelling: the encoding is compact and
deterministic, and readability comes from the demangler, which is where
Rust puts it too.

A C entry point is not encoded. It is not ours, and it keeps the name
it was declared with. The synthesized program entry is named for the C
ABI and is likewise left alone.

For a symbol to be derivable, a type must carry its own arguments, so
`StructType` and `EnumType` gained a `params` range recording the
instantiation they were built with. That is the right home for the
information regardless of symbols: a type that cannot name its own
arguments cannot be printed, and the self-hosting compiler will need to
print types.

## Consequences

- A source name cannot reach the linker, so no user program can collide
  with the C library or with another alcy item.
- A symbol depends only on the signature, so the same program produces
  the same symbols in any lowering order. The kind is taken from the
  receiver rather than from whichever call site reserved a body first.
- Backtraces show encoded names. Reading one needs the demangler; the
  encoder is not designed to be legible, because determinism and
  injectivity are, and brevity follows from both.
- An encoded symbol is longer than the source name, so a binary's
  symbol table grows. The mangled name is a pure function of the
  signature, so identical code is still deduplicated by the linker.
- Nothing is gained for debug info, which is a separate concern.

## Alternatives considered

- **Prefix every user symbol.** Rejected: it stops the collision but
  not the ambiguity. `Vec::at_mut` for `Vec<i32>` and for `Vec<f64>`
  would still collide, and a system that has to disambiguate by
  appending a counter is a system whose symbols depend on emission
  order.
- **Encode the parameter and return types as well.** Rejected as
  redundant. alcy has no overloading, so the signature already
  determines the types uniquely; including them would grow every symbol
  for no additional discrimination. If overloading ever lands, the
  grammar has room for it.
- **Namespace the C runtime instead.** Rejected: it moves the problem
  onto every C function a program imports, which a user cannot edit.
- **Leave symbols as source names and document the hazard.** Rejected.
  The hazard is not a convention a systems language can push onto the
  programs it exists to write.
