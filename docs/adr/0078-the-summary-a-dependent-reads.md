# ADR-0078: The summary a dependent reads

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-18

## Context

ADR-0038 accepted that a `[lib]` builds alone but that a dependent
compiles the dependency's whole closure from source: "No archive
format, no summaries... those arrive with IR serialization." IR
serialization since landed (ADR-0056), and with it the content hashes
and the identity a cache compares (ADR-0072), so the missing half is
the summary a dependent resolves against. The roadmap's item states
what it must carry: signatures, nominal shapes, generic instances, and
spec implementations.

What makes the generic half the hard one is that alcy monomorphizes
from typed AST at the call site: a wall between packages cannot hand
out a declaration of a generic item, because a dependent that calls
one needs the body to instantiate it. A summary that carried bodies
would be a source archive by another name, and one that carried
pre-instantiated code would need the set of instances up front.

## Decision

**A summary is the package's public surface, written in the binary
form the compiler already has.** A library target's IR binary carries
a `SUMMARY` section: one row per public item of the package's own
modules. A function row names the `FUNCTIONS` row whose signature
carries it, a struct or enum row the `TYPES` row of its nominal, and
every row the source name, the module it belongs to, and whether the
item needs its body to be used. The type tables are the artifact's, so a nominal shape a
signature mentions is already there; the module path on every function
row says which modules are the package's own. A bin's artifact carries
none, because nothing depends on a bin.

**Bodies are never carried.** The dependency's object holds them and
the dependent links it, so a summary is a header: the surface a
dependent checks against, not the code it runs.

**A generic item is named but marked unusable.** Its row has no
`FUNCTIONS` index and sets the flag that says the body is what an
instantiation needs. A dependent that reaches one is refused by name
rather than told the item does not exist. The three ways past this -
carrying typed bodies, pre-instantiating a declared set in the
producer, or leaving the boundary closed for generics - are the next
slice's question; the format reserves the answer in one flag.

**Versioning is the binary form's.** The section rides inside ALIR, so
a `MAJOR` bump that a reader must refuse, a `MINOR` a reader may walk
past, and the trailing digest all apply unchanged. The writer emits
the section only when the surface is non-empty, and a reader that
finds none answers empty rather than failing.

## Consequences

What this buys is the format a dependent can resolve against, in the
shape the compiler already writes and reads: the cache stores it with
the rest of the artifact, a cache hit hands it back, and the section
is what the analyzer's consumption slice will read.

What it costs is duplication with the artifact: the type tables and
the surface rows are written twice when a build writes both the object
and the IR. That is the price of keeping one format rather than
inventing a second, and a `[lib]` that is never depended on pays it
too, because the writer's input is the package and not the target.

What is explicitly out of scope: the analyzer consuming a summary in
place of a dependency's source; the generic boundary; multi-object
linking; registry and git fetchers; and lockfiles.

## Staged landing

**Landed:** the `SUMMARY` section and its writer: a `[lib]` target's
IR binary carries the public items of the package's own modules, each
with the code that carries its signature or the flag saying it needs
its body; a bin's carries none. The reader rebuilds the surface, and
refuses a row whose function index or kind is out of range, which the
unit tests pin. The cache hit path rebuilds the surface from the
stored artifact, so a second build sees the same rows.

**Next:** the analyzer registers concrete signatures, nominal shapes
and spec implementations from a summary and skips the dependency's
bodies; then the generic boundary; then export-list trimming, which
decides what a summary may show a dependent that does not import it.
