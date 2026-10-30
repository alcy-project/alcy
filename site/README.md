# The alcy site

The published site is three kinds of page over one shell: the landing
page, the [guide](../docs/guide/) under `/guide/`, and the
[playground](playground/) under `/playground/`. `tools/site.py` assembles
everything into `site/dist/`, and `.github/workflows/site.yaml` deploys
that directory to GitHub Pages. See
`../docs/adr/0058-the-site-its-generator-and-the-guide.md` for why the
site is shaped this way.

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
  `shared/i18n.ts` is the catalog both trees are cut from. The guide's
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
- `ssg/` - the generator: a parser for the Markdown subset the guide is
  written in, the page shell, the link audit, the build-time
  highlighter, and the template stamper. Its tests are in `ssg/test/`.
- `tsconfig.json`, `tsconfig.workers.json`, `tsconfig.ssg.json` - the
  three builds: the pages (DOM), the playground's two workers
  (WebWorker), and the generator (Bun).

The guide's sources live outside the site, under `docs/guide/`, named
`NN-slug.md`: the number is the reading order, the `# heading` is the
title, and the slug (number dropped) is the URL.

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
stamps the labels from `shared/i18n.ts` at build time and the header
links to the same page in the other tree, which means no page renders in
the wrong language first and the guide reads without JavaScript. The
compiled assets (`shared/`, `playground/`) stay in one place; only the
pages are duplicated per tree.

## What the pages owe their readers

The site is served by GitHub Pages: static files, no server, no headers
of our own. Everything below is a property of the built HTML and of what
the page names, so a change that breaks one breaks it in production and
nowhere earlier.

- **The page reads without JavaScript.** The generator stamps the
  article, the highlighting, the labels, and the language; scripts only
  reveal controls and wire them. `ssg/test/no-js.test.ts` holds this:
  every character of the text is in the document, nothing that needs a
  script to work is shown, and no styling comes from a script.
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
  runs the same gate. The numbers are raw bytes, so they do not drift
  with what a proxy chooses to compress.
- **One catalog per tree, one copy of the assets.** `i18n.ts` is the
  source; the build writes `i18n.<lang>.js` and stamps each tree's
  modules to import its own, so a page never carries the other
  language's strings. The compiled assets stay at the root, and every
  tree fetches them from there.
- **Motion is offered, not imposed.** View transitions sit inside
  `@media (prefers-reduced-motion: no-preference)`; with the preference
  set, navigation is an ordinary page load.
- **Keyboard and screen-reader users get the same page.** Landmarks
  come from the elements, the navigation and the search carry names, the
  search dialog announces its result count through a live region, and a
  control that ships hidden is a real button that appears in the tab
  order the moment it is shown. `ssg/test/a11y.test.ts` holds the markup
  contract.
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

`build` needs `bun` and `tree-sitter` on `PATH` (the versions in
`config.toml` are installed into `out/site-cache/`). Every `alcy` fence
in the guide is compiled with the playground's wasm module and every
link is audited; a fence that does not compile or a link that goes
nowhere fails the build.

On the page, every code block can be copied, and the `alcy` ones can be
run and edited in place. The compiler is the playground's wasm module
again, loaded from `playground/` the first time a reader runs something;
a page that is only read downloads none of it. The compiler module
itself is optional for a local build so the pages still assemble without
it, but the Pages workflow always builds it first, so the deployed guide
carries compiled examples.
