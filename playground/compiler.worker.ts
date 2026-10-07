// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Keeps the compiler wasm module alive and answers check/compile requests.
// The module is expensive to load and instantiate, so this worker is
// created once and reused; only the run worker is disposable.

"use strict";

importScripts("wasm-api.js");

interface CompilerRequest {
  type: "request";
  id: number;
  op: "check" | "compile";
  source: string;
}

let compilerPromise: Promise<AlcyCompiler> | null = null;

function notify(status: CompilerStateName, detail = ""): void {
  self.postMessage({ type: "status", status, detail });
}

function loadCompiler(): Promise<AlcyCompiler> {
  if (compilerPromise === null) {
    notify("loading");
    compilerPromise = AlcyWasmApi.loadCompiler({
      glueUrl: new URL("compiler/alcy_playground.js", self.location.href).href,
      wasmDirUrl: new URL("compiler/", self.location.href).href,
      factoryName: "createAlcyPlayground",
    })
      .then((compiler) => {
        notify("ready");
        return compiler;
      })
      .catch((error: unknown) => {
        notify("error", error instanceof Error ? error.message : String(error));
        compilerPromise = null;
        throw error;
      });
  }
  return compilerPromise;
}

self.addEventListener("message", async (event: MessageEvent<CompilerRequest>) => {
  const request = event.data;
  if (request.type !== "request") {
    return;
  }
  const id = request.id;
  try {
    const compiler = await loadCompiler();
    const entry = request.op === "check" ? "alcy_check" : "alcy_compile";
    const result = compiler.call(entry, request.source);
    const response = {
      type: "response",
      id,
      ok: result.ok,
      returnCode: result.returnCode,
      diagnostics: result.diagnostics,
      stats: {
        fileCount: result.fileCount,
        moduleCount: result.moduleCount,
        functionCount: result.functionCount,
      },
      wasm: result.wasm ? (result.wasm.buffer as ArrayBuffer) : null,
    };
    if (response.wasm !== null) {
      self.postMessage(response, [response.wasm]);
    } else {
      self.postMessage(response);
    }
  } catch (error) {
    self.postMessage({
      type: "response",
      id,
      ok: false,
      error: error instanceof Error ? error.message : String(error),
      diagnostics: [],
    });
  }
});
