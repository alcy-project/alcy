# ast

Abstract syntax tree: index-addressed node tables in one arena.

One table per node type (`literals`, `paths`, `conds`, `blocks`,
`types`, `patterns`, `exprs`, `stmts`, `items`), addressed by the
matching `*Idx` types. Edges between nodes are indices, so passes
never downcast. Span arrays and identifier spellings live in `spans`.
Node payloads are untagged unions: parsers set the member matching
`kind` before pushing.

## Entry points

- `AstArena` - the tables. Shared across a whole package; per-file
  arenas would dangle through `ModuleNode::items`.
- `verify_file(arena)` -> `base::Result<void, VerifyError>`: every
  child index in every node is invalid or in range. Parser-built
  arenas satisfy this by construction; hand-built arenas must pass
  before crossing into the analyzer. Pure: no I/O, no logging, no
  bag writes.

## Input requirements

- An invalid index is an allowed absent edge; any other out-of-range
  index is a `VerifyError` naming the offending table.
- `kind` selects the active payload member; reading any other member
  is meaningless even though the union access itself is byte-safe.
