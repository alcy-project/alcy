// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// What a reader who is not looking at the page gets. The generator emits
// the landmarks, the labels, and the current-page state, so these checks
// hold that every page names its regions, every control that ships
// hidden announces why it is unusable, and the language each document
// declares is the language its strings are in.

import { equal, ok, match } from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import type { Block } from "../src/markdown.js";
import { REFERENCE_TREE, renderNotFound } from "../src/render.js";
import { navNames, renderPage } from "./page.js";

const alcy: Block = { type: "code", language: "alcy", text: "fn main() {}", line: 1 };
const prose: Block = { type: "paragraph", text: "hello", line: 1 };

test("each document declares the language of its own strings", () => {
  match(renderPage({ blocks: [prose] }).html, /<html lang="en">/);
  match(renderPage({ language: "ja", blocks: [prose] }).html, /<html lang="ja">/);
});

test("the page's regions are named landmarks", () => {
  const page = renderPage({ blocks: [prose] });
  const tags = page.landmarks.map((landmark) => landmark.tag);
  for (const tag of ["header", "main", "footer"]) {
    ok(tags.includes(tag), `the page has a <${tag}>`);
  }
  // The banner, the main region, and the contentinfo come from the
  // elements themselves; the navigation and the search need names. The
  // header's nav is named first, so a bare `<nav[^>]*aria-label` would
  // be satisfied by the article's own contents and let it stay silent.
  match(page.html, /<nav class="site-nav" aria-label="[^"]+"/);
  ok(navNames(page).length > 0, "the page has a named nav");
});

test("no two landmarks answer to the same name", () => {
  // Repeated regions are leaf nodes in a screen reader's landmark list,
  // so two called "Guide" would be told apart by position alone.
  const page = renderPage({ blocks: [prose] });
  ok(navNames(page).length >= 3, "the header, the contents, and the pager are navs");
  // Every nav carries a name: a missing one would read as `""` and pass
  // a set-comparison while leaving the region silent.
  const silent = navNames(page).filter((name) => name.trim() === "");
  equal(silent.length, 0, `${navNames(page).join(", ")} all name their region`);
  equal(
    new Set(navNames(page)).size,
    navNames(page).length,
    `${navNames(page).join(", ")} name distinct regions`,
  );
});

test("the current page is marked, not just coloured", () => {
  const page = renderPage({ blocks: [prose] });
  match(page.html, /aria-current="page"/);
});

test("a control that ships hidden is still describable when it appears", () => {
  const page = renderPage({ blocks: [alcy] });
  // The row is hidden until the script reveals it, and the buttons are
  // real buttons, so the keyboard reaches them the moment it appears.
  match(page.html, /<div class="code-actions" hidden>/);
  match(page.html, /<button type="button" data-action="copy">Copy<\/button>/);
  match(page.html, /<button type="button" data-action="run">Run<\/button>/);
  match(page.html, /data-action="edit" aria-pressed="false">Edit</);
  // The editing frame is a pressed-state toggle, so it announces as one.
  match(page.html, /data-action="reset"[^>]*hidden/);
});

test("the editing overlay is labelled in the page's language", () => {
  const page = renderPage({ language: "ja", blocks: [alcy] });
  match(page.html, /aria-pressed="false">編集</);
  match(page.html, />コピー</);
});

test("the 404 is marked as a dead end and still reads in order", () => {
  const html = renderNotFound("INLINE_BOOT", { text: "TESTSTAMP", url: "" }, (code) => code);
  match(html, /name="robots" content="noindex"/);
  match(html, /<html lang="en">/);
  ok(html.indexOf("<main") < html.indexOf("<button type=\"button\" data-action=\"copy\""));
});

test("the keyboard reaches the article without the header in the way", () => {
  // The header repeats on every page. The first tab stop is a link past
  // it, the main it points at takes the focus, and it stays reachable
  // without a stylesheet trick that would leave it off-screen forever.
  const page = renderPage({ blocks: [prose] });
  match(page.html, /<body[^>]*>\s*<a class="skip-link" href="#content">/);
  match(page.html, /<main[^>]*id="content"[^>]*tabindex="-1"/);
  // The label is the reader's own language, not the catalog's default.
  match(renderPage({ language: "ja", blocks: [prose] }).html, />本文へ移動<\/a>/);
});

test("the search dialog names its field, its list, and its tally", () => {
  // The dialog is built in the browser; these are the attributes the
  // script sets, held here so a change to them is a change to the
  // page's contract with a screen reader.
  const source = readFileSync("site/shared/search.ts", "utf8");
  match(source, /field\.setAttribute\("aria-label", t\("search\.placeholder"\)\)/);
  match(source, /results\.setAttribute\("aria-label", t\("search\.placeholder"\)\)/);
  match(source, /tally\.setAttribute\("role", "status"\)/);
  match(source, /t\("search\.count", \{ count: String\(results\.length\) \}\)/);
});

test("the reference names its regions the same way the guide does", () => {
  const page = renderPage({ blocks: [prose], tree: REFERENCE_TREE });
  match(page.html, /<nav class="site-nav" aria-label="[^"]+"/);
  match(page.html, /<nav class="guide-nav" aria-label="Reference"/);
  ok(navNames(page).every((name) => name !== ""), "every landmark is named");
  equal(new Set(navNames(page)).size, navNames(page).length, "no two landmarks share a name");
});