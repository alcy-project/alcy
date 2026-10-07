# The alcy site

The published site is three kinds of page over one shell: the landing
page, the [guide](../docs/guide/) under `/guide/`, and the
[playground](playground/) under `/playground/`. `tools/site.py` assembles
everything into `site/dist/`, and `.github/workflows/site.yaml` deploys
that directory to GitHub Pages. See
`../docs/adr/0057-the-site-its-generator-and-the-guide.md` for why the
site is shaped this way.

## Layout

- `index.html` - the landing page.
- `shared/` - the shell every page loads: `boot.js` resolves the color
  theme before first paint, `site.css` holds the design tokens and the
  shared styles, and `site.ts` holds the theme and language stores and
  wires the pickers. The playground's editor and the guide's build-time
  highlighting share the rendering half in `shared/highlight.ts`.
- `playground/` - the browser playground page. Its own README covers the
  page and what it expects from the compiler module.
- `ssg/` - the guide's generator: a parser for the Markdown subset the
  guide is written in, the page shell, the link audit, and the
  build-time highlighter. Its tests are in `ssg/test/`.
- `tsconfig.json`, `tsconfig.workers.json`, `tsconfig.ssg.json` - the
  three builds: the pages (DOM), the playground's two workers
  (WebWorker), and the generator (node).

The guide's sources live outside the site, under `docs/guide/`, named
`NN-slug.md`: the number is the reading order, the `# heading` is the
title, and the slug (number dropped) is the URL.

## Building

```sh
uv run ./tools/site.py build          # assemble site/dist
uv run ./tools/site.py test           # run the generator's tests
uv run ./tools/site.py serve          # http://127.0.0.1:8000/
```

`build` needs `pnpm` and `tree-sitter` on `PATH` (the versions in
`config.toml` are installed into `out/site-cache/`). Every `alcy` fence
in the guide is compiled with the playground's wasm module and every
link is audited; a fence that does not compile or a link that goes
nowhere fails the build. The compiler module itself is optional for a
local build so the pages still assemble without it, but the Pages
workflow always builds it first, so the deployed guide carries compiled
examples.
