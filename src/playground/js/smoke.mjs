// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The playground's smoke test, run by tools/check_playground.py under
// bun: compile a program, run the module it produced, and check that a
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

// The formatter is defined for every input, including one the grammar
// rejects, and what it answers must already be in the canonical layout.
const decoder = new TextDecoder();
const messy = alcy.format('fn main() {println( "hello, playground" )}');
if (!messy.ok || messy.text === null) {
  console.error("format refused to answer");
  process.exit(7);
}
const once = decoder.decode(messy.text);
if (decoder.decode(alcy.format(once).text ?? new Uint8Array()) !== once) {
  console.error("format is not idempotent");
  process.exit(8);
}
const broken = alcy.format("fn {{{{");
if (!broken.ok || broken.text === null) {
  console.error("format refused input the grammar rejects");
  process.exit(9);
}

// The linter answers beside the compiler's own check: a program the
// compiler accepts lints clean, and one it refuses comes back as
// errors through the same diagnostics.
const clean = alcy.lint('fn main() {\n  println("hello, playground")\n}\n');
if (!clean.ok) {
  console.error("lint refused a program the compiler accepts:");
  console.error(JSON.stringify(clean.diagnostics, null, 2));
  process.exit(10);
}
const denied = alcy.lint("fn main() {\n  missing()\n}\n");
if (denied.ok) {
  console.error("lint accepted a program that does not resolve");
  process.exit(11);
}
if (!denied.diagnostics.some((entry) => entry.severity === "error")) {
  console.error("lint rejected without an error diagnostic");
  process.exit(12);
}

// Set rather than exit: the module's output may still be buffered.
process.exitCode = ran.exitCode;
