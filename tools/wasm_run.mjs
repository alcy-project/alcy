// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Runs a WASI preview1 module the way a command-line host does: stdout and
// stderr stay the process's own, `_start` is called, and its exit status
// becomes this process's status. The wasm e2e runner uses it so a case
// compares a module's output and code, not the host API around them.

import { readFileSync } from "node:fs";
import { WASI } from "node:wasi";

const path = process.argv[2];
const wasi = new WASI({
  version: "preview1",
  args: [path],
  env: {},
  returnOnExit: true,
});

const module = new WebAssembly.Module(readFileSync(path));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
const code = wasi.start(instance);
process.exit(code ?? 0);
