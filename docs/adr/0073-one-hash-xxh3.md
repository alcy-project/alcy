# ADR-0073: One hash: XXH3

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-16

## Context

`fnv1a64` is the project's only hash. It is the trailing footer of the
binary IR (the writer and reader share it so a handed-back file is
refused rather than read), the content hash of every `FILES` row, and
the digest `ir::identity_digest` folds over the file table, the pointer
width, and the toolchain. The diagnostic bag hashes its fields with the
same function to accelerate dedup, and the benchmark generator carries
its own copy to label generated sources. The cache ADR-0072 keys
artifacts with the identity, so every one of those hashes is on a build
path.

FNV-1a is byte-serial and its mixing is thin — a few low bits of a short
input barely move — and its output is 64 bits, which is fine for a
dedup accelerator and not fine for a content address once entries are
shared or counted in the millions. The project already vendors
`third_party/xxhash` (0.8.4), `fpag` already links it, and
`src/docs/architecture.md` already lists it among the wrapped
dependencies, so a better hash is not a new dependency.

## Decision

**XXH3 is the project's one hash, and fnv1a64 is removed everywhere in
the project's own code.**

- **Width follows the slot.** A recorded content digest is
  **XXH3-128**: the `FILES` content hash, the `IDENTITY` digest, the
  binary footer, and the cache key. These are content addresses — a
  collision serves a wrong artifact, and a shared cache's entry
  counts make a 64-bit birthday bound a real number. A digest whose
  collisions are settled by comparing the whole stays **XXH3-64**: the
  diagnostic bag's dedup hash and the benchmark generator's label.
- **One facade.** `src/base/hash.h` exposes the hashing the
  project uses: a `Digest128` value (two `u64`s, equality,
  little-endian bytes, hex), one-shot hashing over byte spans, and a
  streaming form for composed digests. `xxhash.h` stays inside the
  facade, so no other module includes a third-party header.
- **The binary IR takes a major bump.** `MAJOR` goes 1 to 2: the
  footer grows to 16 bytes, every `FILES` row carries a 16-byte hash,
  and `IDENTITY` becomes 16 bytes. The form has no production
  consumers, which is what makes this the cheap moment;
  `src/docs/ir-format.md` records the spelling.
- **fpag's `Xxh3Hasher64` is not extended for this.** It is a
  container hasher — 64 bits, string-shaped, for `fpag`'s own users —
  and the compiler needs spans, 128 bits, streaming, and a digest
  type; growing compiler-shaped API on a general library couples the
  two. The compiler depends on `//third_party/xxhash` directly.
- XXH3 is not a cryptographic hash and is not treated as one.
  Adversarial integrity for fetched artifacts is the fetchers era's
  question — provenance and signatures — not this format's.

## Consequences

Every existing pin in the IR tests changes once, together with the
format bump; after that a single hash function and a single digest type
serve the compiler. Hashing all sources on every build stops being a
byte-serial cost, which matters because the cache computes the identity
before it can look anything up.

The standard library's own hash — today a private FNV-1a inside
`Map` — is replaced in-language; that decision is ADR-0074. Out of
scope: a keyed or streaming-configurable XXH3, cryptographic hashing,
and any migration of hashes inside vendored third-party sources, which
are their own projects.
