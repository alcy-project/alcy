# diag

Diagnostics: spans, the diagnostic bag, and rendering.

- `Span` is a half-open byte range `[offset, offset + length)` within
  one source file. Spans never own bytes; they locate views into
  storage owned elsewhere (usually `SourceManager`).
- `DiagBag` accumulates diagnostics. Bag indices are append-only and
  stay valid for the bag's lifetime.
- Rendering turns a diagnostic plus a `SourceFetch` callback into
  human-readable text.

## Entry points

- `DiagBag::emit(severity, code, ...)` -> bag index. Never fails.
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
