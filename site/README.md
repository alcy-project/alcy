# The Alcy site

The published site is three kinds of page over one shell: the landing
page, the [guide](../docs/guide/) under `/guide/`, and the
[playground](playground/) under `/playground/`. `tools/site.py` assembles
everything into `site/dist/`, and `.github/workflows/site.yaml` deploys
that directory to GitHub Pages.

## Layout

- `index.html` - the landing page's template; the generator stamps it
  once per language.
- `shared/` - the shell every page loads: `boot.js` resolves the color
  theme before first paint, inlined into every page's head by the
  generator so no request stands between the HTML and the paint;
  `site.css` holds the design tokens and the
  shared styles, and `site.ts` holds the theme store, the settings
  toggle, the page's language, and the service-worker registration that
  keeps the compiler and tree-sitter assets warm. `sw.ts` builds to
  `sw.js` at the site's root, so its scope covers every tree;
  `tools/site.py` stamps `asset-version.json` beside it, and the worker
  names its cache from that file, so a deploy retires the old cache on
  the next page load. The playground's editor and the guide's
  build-time highlighting share `shared/highlight.ts`, and
  `shared/catalog.json` is the catalog both trees are cut from; the
  build generates one TypeScript module per language beside it. The guide's
  code blocks are two modules: `guide-copy.ts` reveals the row and
  wires Copy on every block, and `guide.ts` adds Run and Edit, with the
  edit colored by the same tree-sitter grammar the playground uses,
  fetched on the first Edit. A page loads the second only when a block
  holds a complete program. Open hands the working copy to the
  playground through `handoff.ts`, and `compiler-client.ts` is the
  shared client for the playground's two workers. `search.ts` opens
  the guide's search dialog from the header's button, `/`, or
  Ctrl/Cmd-K, over the index the generator writes; a result links into
  the tree the reader is in.
- `playground/` - the browser playground. Its `index.html` is a template
  too; the launch scripts resolve the page's assets against their own
  URL, so a page in another language's tree finds the one set of assets.
  Its own README covers the page and the compiler module it expects.

The generator itself is not TypeScript: the library is the C++
module under `../src/ssg/` — a parser for the Markdown subset the
guide is written in, the page shell, the link audit, the build-time
highlighter, and the template stamper — and the tool that drives it
is the `alcy-docsite` binary under `../src/docsite/`, which also
compiles every alcy fence through the compiler it links. The
library's tests are in `../src/tests/ssg/`. `tsconfig.json` and
`tsconfig.workers.json` are the two remaining TypeScript builds: the
pages (DOM) and the playground's two workers (WebWorker).

The guide's sources live outside the site, one directory per language
under `docs/guide/<lang>/`, named `NN-slug.md`: the number is the reading
order, the `# heading` is the title, and the slug (number dropped) is the
URL. The default language (`en`) defines which pages exist; a page missing
from another language falls back to it, so translations can land one page
at a time.

The generator also writes the site's root files: `404.html`, whose links
are written from the deployment root because GitHub Pages serves it
under the path that was asked for; `sitemap.xml` and `robots.txt` from
the page list; `search-index.json`, the guide's pages and sections that
`shared/search.ts` reads; `og.png`, the social card every page cites;
and the icon. Every page carries its own canonical address and the
opposite language's alternate, and its footer names the build: the
commit's short sha, or the tag when the build came from a tagged
commit (`ALCY_BUILD_STAMP` lets CI hand the tag down).

## Languages

Every page exists once per language. English is the tree at the site
root; each other language lives under its prefix, so `/ja/guide/...` is
the Japanese guide and `/ja/playground/` its playground. The generator
stamps the labels from `shared/catalog.json` at build time and the header
links to the same page in the other tree, which means no page renders in
the wrong language first and the guide reads without JavaScript. The
compiled assets (`shared/`, `playground/`) stay in one place; only the
pages are duplicated per tree.

## What the pages owe their readers

The site is served by GitHub Pages: static files, no server, no headers
of our own. Everything below is a property of the built HTML and of what
the page names, so a change that breaks one breaks it in production and
nowhere earlier.

- **The page reads without JavaScript.** The generator stamps the article, the
  highlighting, the labels, and the language; scripts only reveal controls and
  wire them. The doctests under `../src/tests/ssg/` hold this: the article's
  words are in the document, every element that ships `hidden` hides a
  control or a panel and never the article, a page names only the scripts it
  needs (the shell alone, `guide.js` when a block runs, no editor until
  Edit), and the language each document declares is the language its strings
  are in. The page-contract tests state these once, so a template change is
  answered in one place.
- **The first paint needs nothing but the HTML.** `boot.js` is inlined
  into every head and resolves the theme before the body paints, so a
  reader who chose dark never sees a light flash, offline or not.
- **A page loads one module, and it names what it needs.** Each page has
  one entry script at the end of the body; `modulepreload` covers the
  shell and, on a code page, the guide's copy module. Only a page with a
  runnable block names `guide.js`, and only the playground names its
  workers. A new dependency belongs behind an interaction or behind a
  route, never in the shell.
- **Sizes are budgeted.** `site.py budget` holds the pages, the shell,
  the catalogs, and the guide's two modules to byte counts, and `build`
  runs the same check. The numbers are the bytes a reader downloads,
  gzipped the way the server sends them, so they do not drift with what
  a proxy chooses. A CI run publishes the table to its step summary. The
  compiler module is the exception: its size belongs to the compiler, so
  it is held to a ceiling that catches a change in its size class rather
  than a ratchet that trips on every compiler feature.
- **One catalog per tree, one copy of the assets.** `catalog.json` is
  the source; the build writes `i18n.<lang>.js` and stamps each tree's
  modules to import its own, so a page never carries the other
  language's strings. The compiled assets stay at the root, and every
  tree fetches them from there.
- **The chrome is repeated, and that is the cheap trade.** A reading
  page's header and footer gzip to about 830 bytes, and every page
  carries its own copy: twice per language tree, and again for the
  reference. Gzip works per response, and the reader's browser holds
  the previous page's bytes in a window it cannot reuse for this
  page's markup, so the repeat is paid on every navigation. Rebuilding
  the chrome from a script would replace it with a page that renders
  nothing until a module runs. The repeat stays, and `budget` keeps it
  from growing unseen.
- **Motion is offered, not imposed.** View transitions sit inside
  `@media (prefers-reduced-motion: no-preference)`; with the preference
  set, navigation is an ordinary page load.
- **Keyboard and screen-reader users get the same page.** Landmarks
  come from the elements, the navigation and the search carry names, the
  search dialog announces its result count through a live region, and a
  control that ships hidden is a real button that appears in the tab
  order the moment it is shown. The header and settings doctests under
  `../src/tests/ssg/` hold the markup contract.
- **Offline is a cache, not a rewrite.** The service worker caches the
  shell and the assets by name and leaves navigations to the network
  first, so a deploy is seen on the next load and an old cache is retired
  by its version.

## Building

```sh
uv run ./tools/site.py build          # assemble site/dist
uv run ./tools/site.py test           # run the generator's tests
uv run ./tools/site.py budget         # check the built pages' weight
uv run ./tools/site.py serve          # http://127.0.0.1:8000/
```

`build` runs the docsite generator's build first, as a host release build
in `out/docsite/`. Ninja keeps it in step with the compiler and the
standard library the guide's fences compile against; an up-to-date build
is a no-op. The generator is separate from the compiler module that
`--with-compiler` builds: the guide's fences are compiled by the
generator itself, while that module only serves the playground's Check
and Run buttons.

`tree-sitter` (the CLI, for the grammar wasm) and `bun` (TypeScript
and `web-tree-sitter`, the versions in `config.toml` installed into
`out/site-cache/`) stay on `PATH` for the playground. Every `alcy`
fence in the guide is compiled by the generator itself, through the
compiler the binary links, and every link is audited; a fence that
does not compile or a link that goes nowhere fails the build.

On the page, every code block can be copied, and the `alcy` ones can be
run and edited in place. The compiler is the playground's wasm module
again, loaded from `playground/` the first time a reader runs something;
a page that is only read downloads none of it. The compiler module
itself is optional for a local build so the pages still assemble without
it, but the Pages workflow always builds it first, so the deployed guide
carries compiled examples.
