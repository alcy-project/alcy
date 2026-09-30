# Known failures

A fuzz target that always finds a crash is a target nobody runs. This
directory records the failures that are already understood, so
`fuzz/run.py` can tell a new defect apart from the same known one.

`crashes.json` holds the record. There are two kinds of entry, and the
distinction is the whole point:

- **`targets`** — the target cannot be a gate at all, because a
  dependency fails on a *class* of inputs. `fuzz/run.py` reports the
  target as `DISABLED`: never counted as passing, never counted as
  failing, and always visible. Use this when enumerating inputs is
  hopeless, which is the normal case for a third-party parser.
- **`inputs`** — one specific input is expected to fail, keyed on the
  sha256 of its bytes. `fuzz/run.py` suppresses that one hash and
  reports the reason.

`inputs/` holds the bytes for each `inputs` entry, so a developer can
replay them and quote them in an upstream report.

## Policy

Every entry names the layer at fault, and only a dependency's failure
belongs here. A crash in `src` is a bug: reduce it to a unit test under
`compiler/tests`, and add a property test under `src/tests/property` if it
needs many inputs. A vendored dependency that crashes on hostile input
is worth reporting upstream, and the entry should say so.

A `targets` entry is a stronger claim than an `inputs` entry, because it
suppresses an unbounded set. It needs a reproducer that involves no
code from this repository, so the report can be filed without arguing
about it.

## Replaying an entry

    out/fuzz/fuzz_manifest fuzz/known/inputs/<sha256>
    ./fuzz/run.py --replay fuzz/known/inputs/<sha256>

## Dropping an entry

When a dependency is updated, delete the entry and run the target. If it
still crashes, the defect is not what the entry claimed.
