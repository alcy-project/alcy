// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The C ABI the compiler wasm module exposes, wrapped for JavaScript.
// This is a classic script: the compiler worker loads it with
// `importScripts`, so `AlcyWasmApi` lands on the worker's global scope.
//
// The struct layout mirrors `AlcyResult` from
// `compiler/playground/playground.h`, built for wasm32: four-byte
// pointers and `usize`, no padding between the fields. The table below is
// the one place the JS side names it.

"use strict";

interface AlcyCallResult {
  ok: boolean;
  returnCode: number;
  fileCount: number;
  moduleCount: number;
  functionCount: number;
  wasm: Uint8Array | null;
  diagnostics: unknown[];
}

interface AlcyCompiler {
  call(entry: string, source: string): AlcyCallResult;
}

interface AlcyLoadOptions {
  glueUrl: string;
  wasmDirUrl: string;
  factoryName?: string;
}

interface AlcyWasmApiNamespace {
  loadCompiler(options: AlcyLoadOptions): Promise<AlcyCompiler>;
  RESULT_BYTES: number;
}

type CompilerStateName = "idle" | "loading" | "ready" | "error";

// The emscripten module as far as this script uses it.
interface EmscriptenModule {
  HEAPU8: Uint8Array;
  _malloc(size: number): number;
  _free(pointer: number): void;
  _alcy_check?(source: number, length: number, result: number): number;
  _alcy_compile?(source: number, length: number, result: number): number;
  _alcy_release?(result: number): void;
  [name: string]: unknown;
}

type AlcyFactory = (options: Record<string, unknown>) => Promise<EmscriptenModule>;

declare var AlcyWasmApi: AlcyWasmApiNamespace;
interface WorkerGlobalScope {
  AlcyWasmApi: AlcyWasmApiNamespace;
}

(function (global: WorkerGlobalScope) {
  "use strict";

  const RESULT_BYTES = 32;
  // The `AlcyResult` fields as word offsets, in declaration order.
  const OFFSET = {
    wasm: 0,
    wasmLen: 4,
    diagnostics: 8,
    diagnosticsLen: 12,
    fileCount: 16,
    moduleCount: 20,
    functionCount: 24,
    ok: 28,
  } as const;

  const decoder = new TextDecoder("utf-8");

  // Loads the Emscripten module named by `glueUrl`. The compiler build is a
  // classic (non-ES6) MODULARIZE output whose factory is `factoryName` on
  // the global scope; `wasmDirUrl` is the directory holding the sidecar
  // `.wasm` file.
  async function loadCompiler({
    glueUrl,
    wasmDirUrl,
    factoryName = "createAlcy",
  }: AlcyLoadOptions): Promise<AlcyCompiler> {
    importScripts(glueUrl);
    const factory = (global as unknown as Record<string, unknown>)[factoryName];
    if (typeof factory !== "function") {
      throw new Error(
        `${glueUrl} did not define ${factoryName}; the compiler wasm module is missing or built without MODULARIZE`,
      );
    }
    const module = await (factory as AlcyFactory)({
      locateFile: (path: string) => new URL(path, wasmDirUrl).href,
      noInitialRun: true,
      print() {},
      printErr() {},
    });
    return new Compiler(module);
  }

  class Compiler implements AlcyCompiler {
    private readonly module: EmscriptenModule;
    private readonly encoder = new TextEncoder();
    private readonly missing: string[];

    constructor(module: EmscriptenModule) {
      this.module = module;
      this.missing = ["_alcy_check", "_alcy_compile"].filter(
        (name) => typeof module[name] !== "function",
      );
    }

    // Runs `alcy_check` or `alcy_compile` over one source string and
    // returns plain JavaScript values: the diagnostics array, the counts,
    // and a copy of the wasm bytes. Every wasm-side allocation is released
    // before this returns.
    call(entry: string, source: string): AlcyCallResult {
      const module = this.module;
      const name = `_${entry}`;
      const fn = module[name];
      if (typeof fn !== "function") {
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
        const call = fn as (source: number, length: number, result: number) => number;
        const returnCode = call(sourcePtr, sourceBytes.length, resultPtr);
        const view = new DataView(module.HEAPU8.buffer, resultPtr, RESULT_BYTES);
        const wasmPtr = view.getUint32(OFFSET.wasm, true);
        const wasmLen = view.getUint32(OFFSET.wasmLen, true);
        const diagnosticsPtr = view.getUint32(OFFSET.diagnostics, true);
        const diagnosticsLen = view.getUint32(OFFSET.diagnosticsLen, true);
        const result: AlcyCallResult = {
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
            const parsed: unknown = JSON.parse(text);
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
