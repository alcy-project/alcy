// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Keeps the compiler wasm module alive and answers check/compile requests.
// The module is expensive to load and instantiate, so this worker is
// created once and reused; only the run worker is disposable.

"use strict";

importScripts("wasm-api.js");

let compilerPromise = null;

function notify(status, detail = "") {
  self.postMessage({ type: "status", status, detail });
}

function loadCompiler() {
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
      .catch((error) => {
        notify("error", error && error.message ? error.message : String(error));
        compilerPromise = null;
        throw error;
      });
  }
  return compilerPromise;
}

self.addEventListener("message", async (event) => {
  const request = event.data ?? {};
  if (request.type !== "request") {
    return;
  }
  const id = request.id;
  try {
    const compiler = await loadCompiler();
    const entry = request.op === "check" ? "alcy_check" : "alcy_compile";
    const result = compiler.call(entry, request.source ?? "");
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
      wasm: result.wasm ? result.wasm.buffer : null,
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
      error: error && error.message ? error.message : String(error),
      diagnostics: [],
    });
  }
});
