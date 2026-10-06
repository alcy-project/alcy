// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The C ABI the compiler wasm module exposes, wrapped for JavaScript.
// This is a classic script: the compiler worker loads it with
// `importScripts`, so `AlcyWasmApi` lands on the worker's global scope.
//
// The struct layout mirrors `AlcyResult` from
// `docs/adr/0049-the-direct-backends-and-the-playground.md`, built for
// wasm32: four-byte pointers and `usize`, no padding between the fields.
// This is the one place that knows the layout; if the compiler-side
// header changes, only the table below changes.

(function (global) {
  "use strict";

  const RESULT_BYTES = 32;
  const OFFSET = {
    wasm: 0,
    wasmLen: 4,
    diagnostics: 8,
    diagnosticsLen: 12,
    fileCount: 16,
    moduleCount: 20,
    functionCount: 24,
    ok: 28,
  };

  const decoder = new TextDecoder("utf-8");

  // Loads the Emscripten module named by `glueUrl`. The compiler build is a
  // classic (non-ES6) MODULARIZE output whose factory is `factoryName` on
  // the global scope; `wasmDirUrl` is the directory holding the sidecar
  // `.wasm` file.
  async function loadCompiler({ glueUrl, wasmDirUrl, factoryName = "createAlcy" }) {
    importScripts(glueUrl);
    const factory = global[factoryName];
    if (typeof factory !== "function") {
      throw new Error(
        `${glueUrl} did not define ${factoryName}; the compiler wasm module is missing or built without MODULARIZE`,
      );
    }
    const module = await factory({
      locateFile: (path) => new URL(path, wasmDirUrl).href,
      noInitialRun: true,
      print() {},
      printErr() {},
    });
    return new Compiler(module);
  }

  class Compiler {
    constructor(module) {
      this.module = module;
      this.encoder = new TextEncoder();
      this.missing = ["_alcy_check", "_alcy_compile"].filter(
        (name) => typeof module[name] !== "function",
      );
    }

    // Runs `alcy_check` or `alcy_compile` over one source string and
    // returns plain JavaScript values: the diagnostics array, the counts,
    // and a copy of the wasm bytes. Every wasm-side allocation is released
    // before this returns.
    call(entry, source) {
      const module = this.module;
      const name = `_${entry}`;
      if (typeof module[name] !== "function") {
        throw new Error(
          `the compiler wasm module does not export ${entry} (missing: ${this.missing.join(", ") || "unknown"})`,
        );
      }
      const sourceBytes = this.encoder.encode(source);
      const sourcePtr = module._malloc(Math.max(sourceBytes.length, 1));
      const resultPtr = module._malloc(RESULT_BYTES);
      if (sourcePtr === 0 || resultPtr === 0) {
        throw new Error("the compiler wasm module is out of memory");
      }
      module.HEAPU8.set(sourceBytes, sourcePtr);
      module.HEAPU8.fill(0, resultPtr, resultPtr + RESULT_BYTES);
      try {
        const returnCode = module[name](sourcePtr, sourceBytes.length, resultPtr);
        const view = new DataView(module.HEAPU8.buffer, resultPtr, RESULT_BYTES);
        const wasmPtr = view.getUint32(OFFSET.wasm, true);
        const wasmLen = view.getUint32(OFFSET.wasmLen, true);
        const diagnosticsPtr = view.getUint32(OFFSET.diagnostics, true);
        const diagnosticsLen = view.getUint32(OFFSET.diagnosticsLen, true);
        const result = {
          ok: view.getInt32(OFFSET.ok, true) !== 0,
          returnCode,
          fileCount: view.getUint32(OFFSET.fileCount, true),
          moduleCount: view.getUint32(OFFSET.moduleCount, true),
          functionCount: view.getUint32(OFFSET.functionCount, true),
          wasm: wasmPtr && wasmLen ? module.HEAPU8.slice(wasmPtr, wasmPtr + wasmLen) : null,
          diagnostics: [],
        };
        if (diagnosticsPtr && diagnosticsLen) {
          const text = decoder.decode(
            module.HEAPU8.subarray(diagnosticsPtr, diagnosticsPtr + diagnosticsLen),
          );
          try {
            const parsed = JSON.parse(text);
            result.diagnostics = Array.isArray(parsed) ? parsed : [];
          } catch {
            result.diagnostics = [
              {
                severity: "error",
                code: null,
                message: `diagnostics were not valid JSON: ${text.slice(0, 200)}`,
                span: null,
                labels: [],
              },
            ];
          }
        }
        if (typeof module._alcy_release === "function") {
          module._alcy_release(resultPtr);
        }
        return result;
      } finally {
        module._free(sourcePtr);
        module._free(resultPtr);
      }
    }
  }

  global.AlcyWasmApi = { loadCompiler, RESULT_BYTES };
})(self);
