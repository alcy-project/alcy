# ADR-0057: The site, its generator, and the guide

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-08

## Context

The playground at `playground/` is the project's whole web presence: one
page that deploys to GitHub Pages. Users have nothing to read before or
after trying it. `docs/spec/` is normative and written for implementers,
and `docs/adr/` records decisions; neither is a guide, and the single page
has no place to put one.

The page's sources are TypeScript, compiled by the pinned compiler with
pnpm, and its assets resolve against the page's own directory
([ADR-0049](0049-the-direct-backends-and-the-playground.md)). Anything
that publishes more than one page has to keep that property.

## Decision

The published thing is the *site*, and the playground is one page of it.

- `playground/` becomes `site/`, and the page moves under
  `site/playground/`. Its assets (`compiler/`, `vendor/`, `grammar/`,
  `samples/`) move with it, so every URL the page and its workers resolve
  stays relative and unchanged.
- User-facing documentation lives at `docs/guide/` as Markdown named
  `NN-slug.md`: the number is the reading order, the `# heading` is the
  title, and the slug (number dropped) is the URL. The directory is flat;
  a second kind of page gets a directory of its own only when one exists.
  `/guide/` itself is a contents page, not a redirect, so following the
  header's Guide link never flashes an empty page.
- `site/` carries its own generator (`site/ssg/`) rather than a
  documentation framework: a hand-written parser for a documented Markdown
  subset, page templates, navigation, and link checking. No new runtime
  dependency reaches the site.
- `alcy` code fences are checked at build time with the playground's own
  wasm module, and every fence is highlighted at build time with the
  existing tree-sitter grammar. The guide cannot show code that does not
  compile, and reading it needs no JavaScript.
- The generator stamps each code block with its Copy button, and each
  `alcy` one with Edit and Run too; a small script reveals them and
  wires them, so without JavaScript the page is exactly the static
  guide. Run uses the playground's compiler and runner workers, loaded
  on the first run, and Edit is a plain textarea over a scratch copy:
  the published example is never changed.
- The site has one page tree per language. English is the tree at the
  site root; every other language lives under its own prefix
  (`/ja/guide/...`). The landing page and the playground page are
  templates under `site/`, and the generator stamps every page's labels
  from the catalog, so a page is already in its language before any
  script runs. The header links to the same page in the other tree; the
  playground's dynamic strings read the tree's language.
- The color theme stays a site-wide preference through one shared module,
  with `alcy-site-theme` as the key (the playground's old
  `alcy-playground-theme` is read once), resolved before first paint and
  toggled from the header's settings menu.

Out of scope: publishing `docs/spec/` or `docs/adr/`, search, and content
translations. The guide's body is written in English in every tree;
translating it means adding translated sources beside the English ones.
The interface labels are part of the tree.

## Consequences

The site can grow pages without a framework, and the guide's examples are
validated by the same compiler the playground runs. The costs are a
Markdown parser to keep covered by a test corpus, and a generator whose
only consumer is the site it serves.

The rename touches the build tool, the Pages workflow, and the
architecture's layout table. The ADR log and the compiler-side
`compiler/playground/` module keep their names: the first is history, and
the second is the embedding API, not the site.
