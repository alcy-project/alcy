// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Instantiates one program module and runs it. The page terminates this
// worker to stop a program that does not return, so nothing here is kept
// across runs: a new run gets a new worker. The WASI host side lives in
// `wasi.js` so it can be tested without a worker.

"use strict";

importScripts("wasi.js");

self.addEventListener("message", async (event) => {
  const request = event.data ?? {};
  if (request.type !== "run") {
    return;
  }
  try {
    const result = await AlcyWasi.run(request.wasm);
    self.postMessage({ type: "result", ...result });
  } catch (error) {
    self.postMessage({
      type: "error",
      message: error && error.message ? error.message : String(error),
    });
  }
});
