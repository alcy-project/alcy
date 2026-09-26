# path

Canonical path value type.

The canonical form (separator folding, lexical normalization) is
established once at construction, so joining, comparing, or
serializing paths can never produce a non-canonical spelling. Paths
only flow through build setup, never hot paths.

## Entry points

- `Path::from_native(text)` -> `base::Result<Path, PathError>`
  (`PathError::ContainsNul`). The only validation: no NUL bytes.
- `join`, `parent` (bare names parent to `"."`, roots and `"."` to
  themselves), `is_absolute`, `as_view`/`c_str`.

## Input requirements

- No filesystem existence check: a `Path` is a spelling, not a
  claim that anything exists there. Existence is established by the
  I/O that consumes the path (`SourceManager::load`,
  `io::write_file`), which reports its own errors.
