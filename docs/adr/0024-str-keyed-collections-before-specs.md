# ADR-0024: Str-Keyed Collections Before the Spec System

- Subject: the language
- Status: Accepted
- Date: 2026-10-01

## Context

Self-hosting needs name tables: module, type, and symbol resolution are
all lookups from a name to an entry. `Vec` supplies the sequence, but
not the lookup, so `Map` and `Set` are the next foundation.

A general map wants two capabilities from its key: a hash, and
equality. The language has no way to demand either. The plan for that
is `spec`, a bounded implementation mechanism that lets a declaration
be generic over a capability and lets a type opt in — `Hash`, `Eq`,
`Iterator`, `Display`, `Drop`. Designing and landing `spec` is a task
of its own, and it is the task after this one.

There is a second, smaller constraint. A collection hands out
references into its own storage, so it is an early consumer of the
borrow rules that `Vec` and slices established: a view from `get`
borrows the table, insertion takes it exclusively, and growth moves
what is live.

## Decision

**The key is `str` until `spec` exists.** `Map<V>` is parameterized by
its value only. Keys are copied into owned `String`s at insertion, so
the caller's buffer is never borrowed by the table, and hashing and
equality are this module's own: FNV-1a over the bytes, and a byte
walk. No capability is declared, because there is nothing to declare
it with; when `spec` lands, a `Hash`/`Eq`-generic key is the natural
generalization, and this API is its first client.

**`Set` is `Map<u8>`.** Deduplication is the map's job; the wrapper
stores a unit byte per member and reports whether `insert` added one.
A separate open-addressed table would duplicate the probe logic to
save a byte per entry.

**The table is open addressing with linear probing and tombstones.**
A slot is empty, full, or removed. A lookup walks past removed slots
and stops at an empty one; the first removed slot on the walk is where
a missing key lands, so holes are reused. Growth rehashes at three
quarters occupancy counting removed slots, so insertion stays
amortized without a compaction step. This is the layout `Vec`'s
review already exercised: buffer moves and loans are separable, and
the table reuses that reasoning.

**Removal and clearing end the keys they release.** `remove`, `clear`,
and the map's own destructor move the released key out and drop it, so
a table never leaks the `String`s it owns.

**Iteration and `m[key]` index syntax wait for `spec`.** An iterator
is the shape `spec` is designed around, and an index operator needs
the same opt-in machinery. Neither ships with a one-off protocol here.

## Consequences

The key type is fixed: custom key types cannot be used until `spec`
lands, and callers hold keys as `str` or `String`. That is the whole
need of the self-hosting front end, where every key is a name.

The map's methods take `&str` and copy, so inserting through a key
borrowed from another field is legal when the borrow rules allow it.
A borrow of a view from `get` conflicts with a later insertion and
coexists with further reads, which `tools/check_borrow_rules.py`
pins under `map`.
