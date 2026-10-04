# diag

Diagnostics: spans, the diagnostic bag, and rendering.

- `Span` is a half-open byte range `[offset, offset + length)` within
  one source file. Spans never own bytes; they locate views into
  storage owned elsewhere (usually `SourceManager`).
- `DiagBag` accumulates diagnostics. Indices are append-only, except
  that `dedup()` renumbers and `truncate()` drops them, so an index is
  stable until either runs.
- Rendering turns a diagnostic plus a `SourceFetch` callback into
  human-readable text.

## Entry points

- `DiagBag::emit<i18n::Key::Name>(severity, code, [span,] args...)` ->
  bag index. Never fails. The message comes from the catalog, and the
  bag's `Language` decides which text of it. Every catalog's format
  string is checked against the arguments at compile time.
- `DiagBag::emit_untranslated(severity, code, [span,] format, args...)` ->
  the same, for a test that needs a wording of its own. Nothing the
  compiler reports goes through it.
- `DiagBag::at(index)` -> `const Diagnostic*`, `nullptr` when the
  index names no diagnostic. Callers must null-check.
- `DiagBag::label(index, labels)` ->
  `base::Result<void, BagError>` (`BagError::InvalidIndex`).
- `render(diag, out, options, fetch, ctx)` - `fetch` is
  `std::optional<SourceText>(*)(FileId, const void*)`; `std::nullopt`
  renders without a snippet, never crashing.

## Input requirements

- A span whose offset runs past the fetched bytes is invalid: the
  renderer prints the raw offset instead of fabricating a line and
  column.
- `diag::Reported` is the uniform error type for APIs that report
  through the bag: it proves a diagnostic was emitted and carries no
  payload. Callers gate exit codes on `DiagBag::has_errors()`, never
  on the error value.
