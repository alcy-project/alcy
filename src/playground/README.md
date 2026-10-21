# playground

The compiler as an embeddable host API: `alcy_check`, `alcy_compile`, and
`alcy_release` over one source buffer, with the diagnostics as the JSON
array the CLI's `--json` document carries.

The browser page is the first host. Its build is `alcy_backends = []`
with `--target-os=emscripten`, which produces `alcy_playground.js` and
`alcy_playground.wasm` from `executable("alcy_playground")`; the page
calls the factory the launcher exports and then the C ABI through it.
`js/alcy.mjs` is the reference wrapper: it marshals a source string in,
copies the module bytes and diagnostics out, and can run the module it
just compiled through a minimal WASI import object.

A native build compiles the same library, so the API's unit tests need
no wasm toolchain.
