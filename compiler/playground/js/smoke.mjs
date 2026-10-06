// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The playground's smoke test, run by tools/check_playground.py under
// node: compile a program, run the module it produced, and check that a
// rejected source comes back with diagnostics rather than a module. The
// program's own output is forwarded to stdout so the runner can compare
// it, and the process exits with the module's status.

import { createPlayground } from "./alcy.mjs";

const launcher = process.argv[2];
if (launcher === undefined) {
  console.error("usage: smoke.mjs <alcy_playground.js>");
  process.exit(64);
}

const { default: factory } = await import(launcher);
const alcy = await createPlayground({ factory });

const compiled = alcy.compile('fn main() {\n  println("hello, playground")\n}\n');
if (!compiled.ok) {
  console.error("compile refused a valid program:");
  console.error(JSON.stringify(compiled.diagnostics, null, 2));
  process.exit(2);
}
if (compiled.diagnostics.length !== 0) {
  console.error("compile reported diagnostics for a valid program");
  process.exit(3);
}

const ran = await alcy.run(compiled.wasm);
process.stdout.write(ran.stdout);
process.stderr.write(ran.stderr);

const rejected = alcy.check("fn main() {\n  missing()\n}\n");
if (rejected.ok) {
  console.error("check accepted a program that does not resolve");
  process.exit(4);
}
if (!rejected.diagnostics.some((entry) => entry.severity === "error")) {
  console.error("check rejected without an error diagnostic");
  process.exit(5);
}
if (rejected.wasm !== null) {
  console.error("check produced a module");
  process.exit(6);
}

// Set rather than exit: the module's output may still be buffered.
process.exitCode = ran.exitCode;
