# ADR-0042: The pipeline resolves the target, and says why it cannot

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-03

## Context

`compiler/docs/architecture.md` says the `cli` parses argv, validates flag
values, dispatches, and maps exit codes, and performs no semantic validation
of source or manifests. The code said otherwise in three commands: each
probed the target for `alcy.toml` and emitted a `pipeline::DiagCode`
itself, with its own copy of the four ways to phrase "no manifest
found" - one for a named directory, one for the implicit current
directory, and one of each that also suggests `--file`.

The validation that did exist was per verb and did not cover them all.
`build` and `run` rejected a positional that named a source file;
`check` accepted `--file` instead; `compile` accepted any positional.
The pipeline then assumed what validation had not checked:
`build_single_file` derived an unnamed artifact by replacing the target's
extension, with a `DCHECK` that one was present. `alcy compile noext`
aborted - a trap in a debug build, `std::terminate` from
`std::string::replace` under `-fno-exceptions` in a release one.

Three modules also knew what a source file is called: the `cli`, the
source walk, and the module-naming helpers each tested for `.al`.

## Decision

A raw target is resolved by the `pipeline`, and the message for one that
cannot be is written once.

`pipeline::require_package_manifest(ctx, raw, file_hint)` probes for the
manifest, loads it, and reports a target without one. `file_hint` selects
the wording for the one verb that also accepts a single file; the
current-directory wording is chosen from the resolved path rather than
from a flag, because `alcy build` and `alcy build .` are the same
request. `find_package_manifest` is no longer public.

`pipeline::build_single_file` derives an unnamed artifact's path without
assuming anything about the target: the mode's suffix replaces an
extension if the last dot comes after the last separator, is appended if
there is no extension, and is refused with `EI009` when the mode has no
suffix to append - the executable one on POSIX. Appending nothing would
write the artifact over the source at the same path.

`path::has_source_extension` is the one definition of what a source file
is called, and the `cli`, the source walk, and the tests use it.

## Consequences

- No `cli` file names a `pipeline::DiagCode` any more. A command decides
  which verb ran and what to report; the pipeline decides what a target
  is and why it is not that.
- `alcy build .` now reads "current directory", which it always was.
- `alcy compile noext` is a diagnostic instead of a signal, and
  `alcy compile noext -o out` still compiles a file that is not named
  `.al`: the extension decides what an unnamed output is called, not
  what may be compiled.
- A caller of `build_single_file` that leaves the output empty and asks
  for an executable on POSIX gets an error where it used to get a trap.
- `require_package_manifest` reports the missing manifest with the
  pipeline's code (`EI001`) from the pipeline's stage, so the three
  commands cannot drift in wording without drifting in one place.
