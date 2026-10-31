// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The service worker caches only what it names, and a name it does not
// list is fetched straight from the network -- which means it is absent
// when the network is. A module the guide loads but the worker does not
// name therefore works online and fails offline, silently. This test
// follows the guide's imports far enough to notice.

import { match } from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import type { Block } from "../src/markdown.js";
import { renderPage } from "./page.js";

const here = dirname(fileURLToPath(import.meta.url));
const shared = join(here, "..", "..", "..", "shared");

// The names the worker's fetch handler answers from its cache.
function shellNames(): string[] {
  const source = readFileSync(join(shared, "sw.ts"), "utf8");
  const list = /const SHELL_NAMES = \[([^\]]*)\]/.exec(source);
  const body = list?.[1];
  if (body === undefined) {
    throw new Error("SHELL_NAMES is not a single array literal");
  }
  return [...body.matchAll(/"([^"]+)"/g)].map((entry) => entry[1] as string);
}

// The modules a page asks for at the end of its body.
function loadedModules(blocks: Block[]): string[] {
  const html = renderPage({ blocks }).html;
  return [...html.matchAll(/src="[^"]*shared\/([a-z0-9.-]+\.js)"/g)].map(
    (entry) => entry[1] as string,
  );
}

// The build stamps one catalog file per language from `i18n.ts`, so a
// page never asks for the source module's name.
function shippedName(name: string): string {
  return name === "i18n.js" ? "i18n.en.js" : name;
}

// The shared modules a page's own modules import, followed to a fixed
// point: whatever the browser must fetch before the page can run. A
// module imported only inside a function body is deferred by the caller,
// not fetched with the page, so the walk follows top-level imports only.
function importedModules(entry: string): string[] {
  const seen = new Set<string>();
  const queue = [entry];
  while (queue.length > 0) {
    const name = queue.pop() as string;
    if (seen.has(name)) {
      continue;
    }
    seen.add(name);
    const source = readFileSync(join(shared, name.replace(/\.js$/, ".ts")), "utf8");
    for (const found of source.matchAll(/^import[^;]*from "\.\/([a-z0-9-]+)\.js"/gm)) {
      queue.push(`${found[1] as string}.js`);
    }
  }
  return [...seen].map(shippedName);
}

test("every module a guide page runs is one the worker caches", () => {
  const names = shellNames().join(" ");
  const blocks: Block[] = [
    { type: "code", language: "alcy", text: "fn main() {}", line: 1 },
    { type: "code", language: "sh", text: "alcy run main.al", line: 3 },
  ];
  for (const entry of loadedModules(blocks)) {
    for (const name of importedModules(entry)) {
      match(names, new RegExp(`\\b${name.replace(/\./g, "\\.")}\\b`));
    }
  }
});

test("the worker lists no module that does not exist", () => {
  // A stylesheet is itself; `i18n.en.js` and `i18n.ja.js` are stamped
  // from `i18n.ts` by the build, so their source is named differently.
  for (const name of shellNames()) {
    const source = name.endsWith(".css")
      ? name
      : `${name.replace(/\.js$/, "").replace(/\.(en|ja)$/, "")}.ts`;
    match(readFileSync(join(shared, source), "utf8"), /SPDX-License-Identifier/);
  }
});
