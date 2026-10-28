# ADR-0086: The language server caches one analysis per document version

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-28

## Context

Every semantic feature ran the compiler frontend for itself: hover,
navigation, completion, signature help, and inlay hints each built a
fresh pipeline context and checked the buffer again. That is the
dominant cost of a request, and completion runs on nearly every
keystroke, so the same bytes were parsed and checked many times without
changing. ADR-0084 named the fix -- cache the parse and the checked
package per document version -- and left it until the features that
need it existed.

## Decision

- One type owns a single checked reading: `DocumentAnalysis` holds the
  frontend's context, the checked package it produced, and the file the
  buffer was added as. The package borrows the context, so the two are
  built together, never move, and die together; a feature reads them
  through a const reference.
- `AnalysisCache` keys one reading per URI and invalidates it when the
  document's version changes. A request for a buffer that did not change
  reuses the reading; the first request after an edit builds it. A
  closed buffer drops its entry.
- The server owns the cache and builds the reading at most once per
  request. Feature entry points take the `DocumentAnalysis` rather than
  a `Document` alone, so no feature can run the frontend again by
  accident.

## Consequences

The frontend runs once per edit for the semantic features instead of
once per request, which is the cost that mattered for completion and
inlay hints. The cache holds one checked package per open buffer, so
its memory is bounded by the open set, not the workspace.

Diagnostics still run the linter's own single-source entry, which
checks the buffer again for the rules. Pointing the linter's rules at
the cached reading is a separate step: the linter's entry is the
compiler's own, and replacing it means changing what that entry
returns, not adding a second path beside it.
