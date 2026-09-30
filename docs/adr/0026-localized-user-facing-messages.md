# ADR-0026: Localized User-Facing Messages

- Status: Accepted
- Date: 2026-10-01

## Context

Every message a user reads is an English literal at its emission site:
264 `DiagBag::emit` calls across 28 files, the cli report renderer, the
argument parser's help and error text, and the diagnostic renderer's
chrome. A message is therefore only translatable where the author of
the message happens to be, and the wording is spread over call sites
instead of being reviewable in one place.

The compiler cannot read the environment to pick a language. `LANG` and
`LC_ALL` are the user interface of another program, they vary per
shell, container, and CI runner, and a build that renders diagnostics
in a language the invoking pipeline cannot read is a support incident.
A language has to be asked for, and the answer has to be the same
answer on every machine that did not ask.

Translation also cannot be a runtime lookup keyed by a diagnostic code.
Of the 45 codes the spec registers, 24 carry more than one wording, so
the code is not a message identity. The identity is the message, and it
has to exist at the call site.

## Decision

**A message is identified by `i18n::Key`, and the key names the message
rather than the code.** `compiler/i18n/messages.def` lists every
user-visible message once, as `F(Name, "text")`, grouped by the module
that emits it. The list is included with different adapters to build the
`Key` enum, each language's catalog, and the CLI's choices, so the
ordinals, the tables, and the flag values cannot drift apart. The
catalog is a `constexpr` array of `Entry{Key, std::string_view}`, and
`static_assert` pins the entry count and rejects duplicates. Adding a
language adds a table and a static check, never an edit to a call site.

**Translation happens when the diagnostic is emitted, not when it is
rendered.** `DiagBag` carries the invocation's `Language` and formats
the message on the spot, so `Diagnostic` keeps one already-translated
`string_view` and the renderer stays a function of its inputs. A
`Diagnostic` is therefore a record of what the user was told, not a
recipe for telling them, and rendering it twice renders the same text.

**The format string stays in the compiler's hands.** `format_to<K>`
runs every catalog's format string for `K` through
`fmt::format_string<Args...>`, so a missing translation, a placeholder
the arguments do not satisfy, or a specification that does not apply to
an argument is a build error. This is the same guarantee the call site
had with an inline literal, and it is why the catalogs stay in headers.

**`--lang=<LANG>` is a root option, default `en-us`, matched exactly.**
The tag is lowercase kebab-case: the same spelling in the catalog, in
the flag value, and in a bug report. Case folding and aliases are
rejected rather than normalized, so a value that works is a value
anyone can read off the help text. A tag with no catalog is an error
and never a fallback. Nothing reads `LANG`, `LC_ALL`, or any other
environment variable.

**The data source is an in-tree X-macro, not a generated file.** The
catalog's own cost is a table lookup and a compile-time check, both of
which a header gives for free; a generator would add a build step whose
only advantage is translator ergonomics that no translator needs yet.
`Key` and `format_to<K>` are the seam: when a second language arrives
with contributors who do not write C++, the data moves to
`compiler/i18n/lang/<tag>.toml` with a generator that emits the same tables,
and no call site changes.

**Internal diagnostics stay in English.** `DLOG`, `DCHECK`,
`UNREACHABLE`, verifier kind names, and the JSON document's field names
and machine-readable values are read by tools and by the developers who
own the compiler, and are not user-facing text.

## Consequences

A message is added in two places, the catalog and the call site, and
the catalog entry is what a translator sees. The cost of that is one
edit that the compiler cannot make for you; the benefit is that a
wording change no longer rebuilds a translation nobody reads.

A diagnostic's text is fixed at emission, so a host that renders one
bag under two languages gets the same text twice. Nothing in the cli
does that: one invocation is one language.

The language has to be threaded from the cli into every `DiagBag`, and
a `DiagBag` constructed without one is a compile error rather than a
silent English default.

`<LANG>` values are lowercase kebab-case where BCP 47 writes
`en-US`. The deviation is deliberate and visible: the flag is read and
typed by people, and the two spellings of one language in a help text
and a shell history help nobody.
