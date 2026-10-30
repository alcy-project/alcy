# ADR-0132: The standard library's surface is impls and specs

- Subject: the language
- Status: Accepted
- Date: 2026-10-30

## Context

The library's surface has grown by naming. `str` carries
`parse_i64`, `parse_u64`, `parse_i32`, `parse_u32`, `parse_usize`,
and `parse_f64`; `String` duplicates all six as delegations; a typed
read would add a `next_i32` family (ADR-0129). Most of the iterator
adapters are free functions. Primitives have no inherent methods at
all: the checker accepts inherent impls for nominal types, `str`,
slices, and arrays, and refuses them for scalars, so "a method on
`i32`" has no spelling.

These are not the same problem:

- Some behavior belongs to one type and is naturally written once,
  on that type (`i32::abs`, an `f64`'s methods).
- Some behavior is polymorphic and should be named generically
  (`parse` works for every type that can parse itself from text).
- Some behavior is an algorithm over a capability and cannot become
  a method with what the spec system has today.

The direction this record fixes is that the surface is discovered
where the value is, that a type in a name is a missed generic, and
that free functions remain where the machinery's limits show - not
by style.

## Decision

**Behavior is an impl or a spec: an inherent impl for what belongs to
one type, a spec (with a generic spelling) for what is polymorphic,
and a free function only where the spec system cannot express a
method.**

- **Type-specific behavior is an inherent impl on the type.**
  Primitives are included: the checker's inherent-impl rule gains
  `TypeKind::Primitive`, gated exactly like `str`, slices, and
  arrays (staged suite only, ADR-0090). This is a small checker
  change, and it unblocks every future method on a scalar.
- **Polymorphic behavior is a spec plus implementations, named
  generically.** The `parse` family is the first: `parse_i64`/
  `parse_u32`/... are replaced by the capability's calls -
  `i64::parse(text)` and `T::parse(text)` (ADR-0133) - and
  `String`'s six delegations collapse into one. A typed read
  (ADR-0129) is the same capability: `next::<T>()`.
  - The capability needs a receiver-less (associated) function;
    today every spec method takes a receiver. That record lands
    just before its implementation.
  - Amended at that landing (ADR-0133): the method spelling
    `str.parse::<T>()` did not land with it; a generic method with
    its own parameters on a builtin compound target (`impl str`) is
    the slice it waits on, and the two type-first forms are what
    replaces the six names until then.
- **An algorithm over a spec stays a free function while the spec
  system cannot express it as a method.** The iterator adapters
  (`all`, `fold`, `position`, ...) are the example: specs have no
  default methods and there are no blanket impls for a capability
  over a bounded parameter, so the method form has no spelling.
  This record names the door (spec default methods, or a deliberate
  blanket-impl design) without promising it; the free functions are
  the honest shape today.
- **The naming rule**: a function's name does not encode a type a
  generic parameter or a capability can carry (`parse_i32`,
  `next_i32`, a future `write_i32`). Distinct algorithms keep
  distinct names - `count_chars` and `count_graphemes` are different
  work, not one work on two types.
- **`num` follows the inherent shape.** `core/num.al` (or its
  eventual split) holds per-type impls - `impl i8 { ... }` through
  `impl u64`, floats separately - with the methods Rust's
  `core::num` has (bounds, `abs`, the checked/saturating families,
  bit counts). Methods land with their first callers; a sealed
  `Int`/`Float` capability waits for the first generic algorithm
  that needs one, and the `NonZero`/`Wrapping`/`Saturating`
  newtypes wait with it.
  - `isize` and `usize` are the pointer-width spelling of a fixed
    type: the implementation of that width serves them, and the
    per-width impl text is written out (there are no macros to
    generate it - the cost is accepted).
- Pre-1.0: the concrete spellings are replaced, not aliased.

## Consequences

- The surface is discovered per type (`x.abs()`, `s.parse::<i32>()`,
  `sc.next::<u64>()`), and `str`/`String` stop carrying parallel
  method lists.
- The library gets more text per type (one block each, no macro to
  compress them) in exchange for one shape; readers and the LSP
  find the behavior with the type.
- A free function in the standard library is now a visible note that
  the spec system's limits were reached, which is information a
  future language change can use.

## Alternatives considered

- **Generic everything.** Rejected: `abs` is not polymorphism, and a
  sealed `Int` capability adds dispatch machinery for callers that
  do not exist yet (the first-caller rule).
- **Free functions for type-specific behavior (`num::abs(x)`).**
  Not taken: the method form matches the language's precedent
  (`impl str`) and how the surface is read.
- **Renaming only, without the policy.** Rejected: the rename is
  evidence of the rule, not the rule; the next such function would
  otherwise repeat the shape.
- **Blanket impls now, to make the iterator adapters methods.**
  Not taken: it is a coherence redesign (there are no covered types
  today), and none of the current work depends on it.
