# The build cache

A package build stores its lowered binary IR under one digest over the
inputs that steer it; a later build with the same digest loads the IR
instead of running the frontend. Designed in
`docs/adr/0072-a-packages-cache-holds-its-binary-ir.md`, hashed per
`docs/adr/0073-one-hash-xxh3.md`.

## What is cached, and what is not

The entry is the checked, lowered, **unoptimized** IR: the profile
(`optimize`, `overflow-checks`, `on-panic`), the emit mode, and the
backend all act after the IR exists, so one entry answers every
profile, mode, and backend. What a hit skips is lexing, parsing, name
resolution, checking, and lowering; discovery still loads the sources
(the key hashes them, and the backend reads spans through them), and
the optimizer, the backend, and the link run exactly as after a miss.

`check` keeps no cache: its report is the result. `run` walks the
front half a build walks - the same lookup and the same
store-on-quiet-miss - so a run after a build hits the entry the build
wrote. Single-file `compile` has no manifest and never consults.

## The key

XXH3-128 over, in a fixed order: the `IDENTITY` digest over the loaded
sources, the pointer width, and the compiler version; the XXH3-128 of
the manifest's bytes; and the named target triple. An entry's name is
the key's 32 hex digits plus `.irb`.

Only a quiet lowering stores: a lowering that reported any diagnostic
writes no entry, so a hit rebuilds no diagnostics and never changes
what an envelope reports. A `--no-cache` invocation neither looks nor
stores, and `check` keeps no cache at all: its report is the result.

## On disk

Entries live under the package's ignored output directory,
`<package>/out/.cache/<key>.irb`, beside the artifacts rather than in
them; `rm -rf out` stays the clean gesture. Only the goal package's
cache is read. A store writes to `<key>.irb.part` and renames it into
place, so a hit never reads a half-written file; a file that fails to
load is a miss, not an error, so the cache heals itself by rebuilding.

## Across the hit

A hit moves the entry's storage **and** its string table into the
build: the backend resolves names through the context's table, so the
two travel together and an entry always agrees with the table beside
it. Spans resolve through the freshly discovered sources, whose
loading order is deterministic. Statistics the frontend would have
counted stay zero on a hit; the envelope says `hit`, which is what
tells the two builds apart.

## Reporting

The envelope carries the outcome for the whole build: `off` when the
cache was not consulted, `miss` when the frontend ran, `hit` when it
was skipped, with a miss dominating a mix of targets. The result line
names a consulted cache (`cache hit`, `cache miss`) and stays silent
otherwise; the JSON document always carries `"cache"`.
