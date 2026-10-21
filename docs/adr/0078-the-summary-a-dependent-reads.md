# ADR-0078: The summary a dependent reads

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-18
- Amended: 2026-10-21, the generic boundary (ADR-0079) and the body's
  place in the artifact

## Context

ADR-0038 accepted that a `[lib]` builds alone but that a dependent
compiles the dependency's whole closure from source: "No archive
format, no summaries... those arrive with IR serialization." IR
serialization since landed (ADR-0056), and with it the content hashes
and the identity a cache compares (ADR-0072), so the missing half is
the summary a dependent resolves against. The roadmap's item states
what it must carry: signatures, nominal shapes, generic instances, and
spec implementations.

The generic half looked hard because alcy monomorphizes from typed
AST at the call site, so a declaration of a generic item was thought
not to cross a package boundary: a dependent that calls one needs the
body to instantiate it, and the artifact carried no body. That
reasoning was wrong in two places. The artifact the writer produces
carries the bodies already - the `.summary` sidecar is the whole
serialized package, and the sidecar is what a dependent reads. And a
body that cannot be carried is not a wall but a representation gap: the
IR's type tags name concrete types, so a body of a generic item has
nowhere to put its parameter. A summary that carried bodies would not
be a source archive by another name either: a checked, lowered body is
not source, and it is what the artifact already holds.

## Decision

**A summary is the package's public surface, written in the binary
form the compiler already has.** A library target's IR binary carries
a `SUMMARY` section: one row per public item of the package's own
modules. A function row names the `FUNCTIONS` row whose signature
carries it, a struct or enum row the `TYPES` row of its nominal, and
every row the source name, the module it belongs to, and the symbol
its code is written under. The type tables are the artifact's, so a
nominal shape a signature mentions is already there; the module path
on every function row says which modules are the package's own. A
bin's artifact carries none, because nothing depends on a bin.

**A monomorphic body is not carried in a summary.** The dependency's
object holds it and the dependent links it, so a summary is a header
for the code that is already monomorphic: the surface a dependent
checks against, not the code it runs. A generic body is a different
case and is carried, because no concrete object can hold it; ADR-0079
decides how.

**A generic item is named, and today it is not usable.** Its row has
no code to point at and sets the flag that says an instantiation needs
the body. The reader drops such a row, so a dependent that names one
reads "Unresolved import", which is honest but does not say why.
ADR-0079 replaces the flag with the body itself.

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

What is explicitly out of scope: registry and git fetchers, and
lockfiles. The analyzer's consumption of a summary, the nominal
boundary, multi-object linking, and the generic boundary have since
landed or been decided: the first three are in the staged landing
below, and the generic boundary is ADR-0079.

## Staged landing

**Landed:** the `SUMMARY` section and its writer: a `[lib]` target's
IR binary carries the public items of the package's own modules, each
with the code that carries its signature or the flag saying it needs
its body; a bin's carries none. The reader rebuilds the surface, and
refuses a row whose function index or kind is out of range, which the
unit tests pin. The cache hit path rebuilds the surface from the
stored artifact, so a second build sees the same rows.

**Landed: a dependency resolves against the summary.** A library's
build writes its summary beside the artifact (`out/<name>.summary`, the
same binary form), and a path dependency loads it in place of its
sources when it is there and fresh: the recorded identity is recomputed
over the files it names while they are all present, and a package
shipped without its sources is answered from the summary alone. The
surface is spelled into the syntax arena - free functions over
primitives, references, tuples, arrays, slices, and function views for
now - and the checker registers it the way it registers source, export
trimming included. Each row carries the symbol its code is written
under, so a call into the package names what the library's own artifact
exports, the dependent's build links that artifact, and the program runs
with the library's sources gone. The runtime's entry points and the two
argument globals are discardable definitions, so two objects in one
image keep one copy of each.

**Landed: a nominal crosses whole.** A struct row carries the fields
its declaration has - name and visibility, in declaration order - and
the surface spells a nominal row into the declaration its module holds,
so a dependent names the type, constructs a literal, and reads a field
with only the summary in hand. A signature that names a nominal spells
too: the row spells the declaration a copy repeats when the shapes
match, and a call crosses it as the value the callee takes. A nominal
parameter, a nominal return, a nested nominal field, and a reference to
a nominal all resolve, link against the library's object, and run with
the library's sources gone. An associated function or method crosses
too: a row names the nominal it belongs to, the surface groups those
rows into the `impl` the declaration's module holds, and a method's
receiver travels as its first argument. The member's symbol names the
nominal it is declared on, so two impls of one module that declare a
same-named member stay apart.

**The export list trims a summary the way it trims source.** An
unexported module is refused on both paths, an exported surface
resolves on both, and the counts agree (probed with a two-module
library, its sources present and then gone). The trim is the resolver's
`PackageRoot.exports`, so a summary never shows a dependent more than
the source it stands in for.

**The generic boundary is decided: ADR-0079.** A generic item crosses
as its declaration and its checked body, and the dependent instantiates
it. The artifact carries no parameterized types yet - its tags are
concrete, and a generic declaration's shape exists only per
instantiation - so the `needs_body` row is still omitted from the
surface, and a dependent that names such an item still reads
"Unresolved import". ADR-0079 is the work that ends that.
