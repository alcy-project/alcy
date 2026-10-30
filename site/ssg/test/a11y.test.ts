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

import type { GuidePage } from "../src/guide.js";
import type { Block } from "../src/markdown.js";
import {
  GUIDE_TREE,
  REFERENCE_TREE,
  renderGuidePage,
  renderNotFound,
} from "../src/render.js";

function page(language: "en" | "ja", blocks: Block[], path = "guide/x.html"): GuidePage {
  return {
    sourceRel: "01-x.md",
    path,
    route: language === "en" ? path : `${language}/${path}`,
    language,
    order: 1,
    title: "Title",
    blocks,
  };
}

function render(language: "en" | "ja", blocks: Block[]): string {
  const target = page(language, blocks);
  return renderGuidePage(target, {
    pages: [target],
    tree: GUIDE_TREE,
    highlight: (code) => code,
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "https://example.test/commit/TESTSTAMP" },
  });
}

const alcy: Block = { type: "code", language: "alcy", text: "fn main() {}", line: 1 };
const prose: Block = { type: "paragraph", text: "hello", line: 1 };

test("each document declares the language of its own strings", () => {
  match(render("en", [prose]), /<html lang="en">/);
  match(render("ja", [prose]), /<html lang="ja">/);
});

test("the page's regions are named landmarks", () => {
  const html = render("en", [prose]);
  match(html, /<header class="site-header"/);
  match(html, /<main/);
  match(html, /<footer class="site-footer"/);
  // The banner, the main region, and the contentinfo come from the
  // elements themselves; the navigation and the search need names. The
  // header's nav is named first, so a bare `<nav[^>]*aria-label` would
  // be satisfied by the article's own contents and let it stay silent.
  match(html, /<nav class="site-nav" aria-label="[^"]+"/);
  match(html, /<nav[^>]*aria-label="/);
});

test("no two landmarks answer to the same name", () => {
  // Repeated regions are leaf nodes in a screen reader's landmark list,
  // so two called "Guide" would be told apart by position alone.
  const html = render("en", [prose]);
  const names = Array.from(html.matchAll(/<nav[^>]*aria-label="([^"]*)"/g), (m) => m[1] ?? "");
  ok(names.length >= 3, "the header, the contents, and the pager are navs");
  // Every nav carries a name: a missing one would read as `""` and pass
  // a set-comparison while leaving the region silent.
  const silent = names.filter((name) => name.trim() === "");
  equal(silent.length, 0, `${names.join(", ")} all name their region`);
  equal(new Set(names).size, names.length, `${names.join(", ")} name distinct regions`);
});

test("a control that ships hidden is still describable when it appears", () => {
  const html = render("en", [alcy]);
  // The row is hidden until the script reveals it, and the buttons are
  // real buttons, so the keyboard reaches them the moment it appears.
  match(html, /<div class="code-actions" hidden>/);
  match(html, /<button type="button" data-action="copy">Copy<\/button>/);
  match(html, /<button type="button" data-action="run">Run<\/button>/);
  match(html, /data-action="edit" aria-pressed="false">Edit</);
  // The editing frame is a pressed-state toggle, so it announces as one.
  match(html, /data-action="reset"[^>]*hidden/);
});

test("the editing overlay is labelled in the page's language", () => {
  const html = render("ja", [alcy]);
  match(html, /aria-pressed="false">編集</);
  match(html, />コピー</);
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
  const html = render("en", [prose]);
  match(html, /<body[^>]*>\s*<a class="skip-link" href="#content">/);
  match(html, /<main[^>]*id="content"[^>]*tabindex="-1"/);
  // The label is the reader's own language, not the catalog's default.
  match(render("ja", [prose]), />本文へ移動<\/a>/);
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
  const target = page("en", [prose]);
  const html = renderGuidePage(target, {
    pages: [target],
    tree: REFERENCE_TREE,
    highlight: (code) => code,
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "" },
  });
  match(html, /<nav class="site-nav" aria-label="[^"]+"/);
  match(html, /<nav class="guide-nav" aria-label="Reference"/);
  const names = Array.from(html.matchAll(/<nav[^>]*aria-label="([^"]*)"/g), (m) => m[1] ?? "");
  ok(names.every((name) => name !== ""), "every landmark is named");
  equal(new Set(names).size, names.length, "no two landmarks share a name");
});

