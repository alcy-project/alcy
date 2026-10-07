# The alcy playground

A browser playground for alcy: edit a program, have it checked, and run it,
with no server behind the page. The site is plain HTML, CSS, and
JavaScript; the compiler is the project's own wasm module, built without
LLVM through the direct wasm backend described in
[ADR-0049](../docs/adr/0049-the-direct-backends-and-the-playground.md).

The built site is deployed to GitHub Pages by
[`.github/workflows/playground.yaml`](../.github/workflows/playground.yaml).

## Layout

- `index.html`, `style.css` - the page.
- `app.ts` - the wiring: it builds the modules, connects their callbacks,
  and starts the flows.
- `editor.ts` - the textarea, the highlighted mirror, the line-number
  gutter, and the geometry that keeps the three in step. Highlighting
  starts once the page is idle, so first paint and typing never wait for
  the tree-sitter wasm; the mirror shows plain text until the colors are
  ready.
- `problems.ts` - the diagnostics list, including jump-to-span.
- `output.ts` - the run meta line and the program's two streams.
- `runtime.ts` - the compiler worker, the disposable runner worker, and
  the Check and Run flows.
- `samples.ts` - the example picker and the restore-or-default choice.
- `session.ts` - the buffer and selected example kept in `localStorage`.
- `status.ts` - the status line, the compiler indicator, and the Run
  button's busy state.
- `tabs.ts`, `splitter.ts`, `theme.ts`, `language.ts` - the side panel's
  tabs, the pane divider, the color theme, and the language.
- `elements.ts`, `state.ts`, `types.ts` - the page's elements, its shared
  state, and the shapes that cross module boundaries.
- `highlight.ts` - tree-sitter highlighting through `web-tree-sitter`.
- `textutil.ts` - byte-offset to UTF-16 conversion for diagnostics.
- `i18n.ts` - the page's own strings, in English and Japanese. Adding a
  language is one catalog there; the `data-i18n` attributes and `t()`
  calls are the only consumers.
- `wasm-api.ts` - the JavaScript side of the compiler's C ABI.
- `compiler.worker.ts` - owns the compiler wasm module (expensive to load,
  reused across requests).
- `runner.worker.ts` - instantiates one program and runs it (disposable, so
  a program that does not return can be terminated).
- `wasi.ts` - the WASI preview1 host side for one program, kept apart from
  the worker so it is testable on its own.
- `samples/` - the examples shown in the picker, with `samples.json` naming
  them. An entry either names a file here or points at a program under the
  repository's `samples/` suite, which is copied under its site name.

The sources are TypeScript. `tools/playground.py build` compiles them to
`playground/build/` with the pinned `tsc` and copies the JavaScript and
its source maps into `playground/dist/` next to the generated pieces.
`build/`, `dist/`, and the vendored binding types are not committed.

## Building and serving

Prerequisites: `pnpm` and `tree-sitter` on `PATH`, Node for `tsc`,
Emscripten (`emcc`) for the compiler module, and `uv` as everywhere else
in the repository. `config.toml` pins the tree-sitter CLI, the
web-tree-sitter binding, the TypeScript compiler, and pnpm. The CLI must
already be on `PATH`; the binding and the compiler are installed with the
pinned pnpm into `out/playground-cache/`. Install pnpm at the pinned
series with `corepack enable pnpm` or a standalone install.

```sh
# Highlighting only; the compiler module is missing and Check/Run stay off.
uv run ./tools/playground.py build

# Build the compiler wasm first (the `playground` GN target, which carries
# the direct wasm backend and no LLVM), then assemble the site.
uv run ./tools/playground.py build --with-compiler

uv run ./tools/playground.py serve          # http://127.0.0.1:8000/
```

`build` reuses the compiler module it finds under
`out/playground/alcy_playground.js` unless `--with-compiler` builds one
first; `--compiler-dir` points it at artifacts somewhere else.
`--with-compiler` defaults to the `playground` target and passes
`alcy_backends=[]` to GN; both are overridable.

The assembled directory is:

```text
dist/
  index.html, style.css, app.js, app.js.map, ...
  compiler/   alcy_playground.js, alcy_playground.wasm
  vendor/     web-tree-sitter.js, web-tree-sitter.wasm
  grammar/    tree-sitter-alcy.wasm, highlights.scm
  samples/    index.json and the example programs
```

## What the page expects from the compiler module

The module's C ABI is `AlcyResult`, `alcy_check`, `alcy_compile`, and
`alcy_release`, defined in `compiler/playground/playground.h`;
`wasm-api.js` carries the site's copy of the struct layout (wasm32:
four-byte pointers, no padding) and is the file to touch if the header
changes. `alcy_compile` returns the program as a final wasm module, not an
object, and `diagnostics` is the `--json` envelope's diagnostic array as
UTF-8 JSON. `compiler/playground/js/alcy.mjs` is the compiler side's
reference wrapper; the site has its own adapter so the worker split and
the WASI host stay under `playground/`.

The module must be the Emscripten `MODULARIZE` build from
`compiler/playground/BUILD.gn`, whose factory is the global
`createAlcyPlayground` (classic glue, not `EXPORT_ES6`), with the sidecar
`.wasm` next to the glue script, built with `-sENVIRONMENT=web,worker,node`
so the same artifact runs in the page's worker and in node's smoke test.

The compiled program speaks WASI preview1: it exports `_start` and its
memory, and imports `fd_write` and `proc_exit` from
`wasi_snapshot_preview1`. `wasi.js` supplies those two imports, so the
same module also runs under `node` and `wasmtime`.

## Deploying

The workflow builds the compiler module, assembles the site, and deploys
`playground/dist` with the Pages actions. Two one-time repository settings
are required:

- Settings → Pages → Build and deployment → Source: **GitHub Actions**.
- The `github-pages` environment, which the workflow uses, is created on
  first deploy.

The site is served from `https://alcy-project.github.io/alcy/`.

## Compiler artifacts

The site consumes exactly two files from the compiler build:
`alcy_playground.js` and its `alcy_playground.wasm` sidecar, which the
wasm release archive also carries under those names. The Pages workflow
builds them with
`tools/build.py --target=playground --target-os=emscripten
--gn-arg='alcy_backends=[]'`, runs `tools/check_playground.py` over the
result, and only then assembles the site, so a broken module fails in CI
rather than in the browser.
