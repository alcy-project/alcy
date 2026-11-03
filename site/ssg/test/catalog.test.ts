// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Which strings a page's scripts read. One set of shared modules serves
// every tree, so the catalog cannot be baked into a module: the page's
// import map names the module for the tree it was generated into, and a
// page that names the wrong one -- or none -- loads the scripts with no
// strings at all, which stops them at the first label they try to read.
// The build refuses such a page; these tests hold the parts it reads.

import { equal, match, ok } from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import type { Language } from "../../shared/i18n.js";
import { translate } from "../../shared/i18n.js";
import {
  CATALOG_SPECIFIER,
  GUIDE_TREE,
  REFERENCE_TREE,
  renderNotFound,
  renderTreeIndex,
} from "../src/render.js";
import { makePage, renderPage } from "./page.js";

const here = dirname(fileURLToPath(import.meta.url));
const source = join(here, "..", "..", "..", "shared");

// The import map a page carries, read back as the pairs it states.
function imports(html: string): Record<string, string> {
  const map = /<script type="importmap">([\s\S]*?)<\/script>/.exec(html);
  const body = map?.[1];
  if (body === undefined) {
    throw new Error("the page carries no import map");
  }
  return (JSON.parse(body) as { imports: Record<string, string> }).imports;
}

// Where the page's first module load sits, by the markers that start one.
const FIRST_MODULE = /type="module"|rel="modulepreload"/;

test("a guide page names the catalog of its own tree", () => {
  equal(imports(renderPage({ blocks: [] }).html)[CATALOG_SPECIFIER], "../shared/i18n.en.js");
  equal(
    imports(renderPage({ blocks: [], language: "ja" }).html)[CATALOG_SPECIFIER],
    "../../shared/i18n.ja.js",
  );
});

test("the tree index names the catalog of the tree it lists", () => {
  const stamp = { text: "TESTSTAMP", url: "https://example.test/commit/TESTSTAMP" };
  const en = renderTreeIndex([], "en", "INLINE_BOOT", stamp, GUIDE_TREE);
  const ja = renderTreeIndex([], "ja", "INLINE_BOOT", stamp, GUIDE_TREE);
  equal(imports(en)[CATALOG_SPECIFIER], "../shared/i18n.en.js");
  equal(imports(ja)[CATALOG_SPECIFIER], "../../shared/i18n.ja.js");
});

test("the not-found page names a catalog too", () => {
  const stamp = { text: "TESTSTAMP", url: "https://example.test/commit/TESTSTAMP" };
  const page = renderNotFound("INLINE_BOOT", stamp, (code) => code);
  equal(imports(page)[CATALOG_SPECIFIER], "shared/i18n.en.js");
});

test("the map comes before the first module the page loads", () => {
  // An import map only governs loads that happen after it is parsed.
  for (const language of ["en", "ja"] as Language[]) {
    const html = renderPage({ blocks: [], language }).html;
    const map = html.indexOf('<script type="importmap">');
    const load = FIRST_MODULE.exec(html);
    ok(load !== null, `${language} page loads a module`);
    ok(map >= 0 && map < (load?.index ?? 0), `${language} page maps before loading`);
  }
});

test("the shared module imports the name the pages answer for", () => {
  // The one import is what a page's map resolves; a name the two do not
  // share would leave the browser with an unresolvable specifier.
  const module = readFileSync(join(source, "site.ts"), "utf8");
  match(module, new RegExp(`^import \\* as catalog from "${CATALOG_SPECIFIER}";$`, "m"));
});

test("a language the loaded catalog lacks falls back instead of throwing", () => {
  // The build prunes a module to one language, so a lookup can miss;
  // it must not be able to stop a page before its controls are wired.
  const missing = "fr" as Language;
  equal(translate(missing, "site.guide"), "Guide");
  ok(translate("ja", "site.guide") !== "Guide", "a carried catalog answers");
});
