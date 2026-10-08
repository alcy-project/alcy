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
  theme before first paint, `site.css` holds the design tokens and the
  shared styles, and `site.ts` holds the theme store, the settings
  toggle, and the page's language. The playground's editor and the
  guide's build-time highlighting share `shared/highlight.ts`, and
  `shared/i18n.ts` is the one catalog the generator stamps from.
  `guide.ts` wires the guide's code blocks -- Copy everywhere, Run and
  Edit on the `alcy` ones -- and `compiler-client.ts` is the shared
  client for the playground's two workers.
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

## Languages

Every page exists once per language. English is the tree at the site
root; each other language lives under its prefix, so `/ja/guide/...` is
the Japanese guide and `/ja/playground/` its playground. The generator
stamps the labels from `shared/i18n.ts` at build time and the header
links to the same page in the other tree, which means no page renders in
the wrong language first and the guide reads without JavaScript. The
compiled assets (`shared/`, `playground/`) stay in one place; only the
pages are duplicated per tree.

## Building

```sh
uv run ./tools/site.py build          # assemble site/dist
uv run ./tools/site.py test           # run the generator's tests
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
