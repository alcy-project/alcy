// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// What a reader gets with JavaScript switched off. The guide is readable
// without it by construction -- the generator stamps the article and the
// highlighting, and the scripts only reveal controls and wire them -- so
// these checks hold that property rather than a particular markup shape:
// every character of the page's text is in the document, no control that
// needs a script to work is shown, and nothing the page shows is styled
// by a script.

import { equal, ok, match } from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import type { Block } from "../src/markdown.js";
import { REFERENCE_TREE, renderNotFound, renderTreeIndex } from "../src/render.js";
import { GUIDE_TREE } from "../src/render.js";
import { makePage, readPage, renderPage } from "./page.js";

const prose: Block = { type: "paragraph", text: "hello reader", line: 1 };
const alcy: Block = { type: "code", language: "alcy", text: "fn main() {}", line: 1 };
const sh: Block = { type: "code", language: "sh", text: "alcy run main.al", line: 3 };

test("the article's prose is in the document, not assembled by a script", () => {
  const page = renderPage({
    blocks: [
      { type: "heading", level: 1, text: "Title", line: 1 },
      prose,
    ],
  });
  ok(page.text.includes("hello reader"), "the paragraph text is in the body");
  ok(page.text.includes("Title"), "the heading text is in the body");
});

test("code is highlighted into the markup the reader receives", () => {
  const page = renderPage({
    blocks: [alcy],
    highlight: (code) => `<span class="hl-keyword">${code}</span>`,
  });
  match(page.html, /<pre><code><span class="hl-keyword">fn main\(\) \{\}<\/span><\/code><\/pre>/);
});

test("a code block's text survives with its buttons hidden", () => {
  const page = renderPage({ blocks: [alcy] });
  ok(page.text.includes("fn main() {}"), "the program is readable");
  // Every control that needs a script carries `hidden` in the markup, so
  // a reader without one never sees a button that does nothing.
  match(page.html, /<div class="code-actions" hidden>/);
  // Only the block's own controls: the header's search button is the
  // page's furniture and needs no script to open its place in the page.
  // The buttons sit inside the group the markup hides; a button that is
  // itself hidden is one a script reveals later (Reset, once editing).
  const article = page.html.slice(page.html.indexOf('class="guide-article"'));
  const group = article.match(/<div class="code-actions" hidden>([\s\S]*?)<\/div>/);
  ok(group !== null, "the block's controls are in a hidden group");
  const actions = (group?.[1] ?? "").match(/<button[^>]*>/g) ?? [];
  ok(actions.length > 0, "the block has controls");
  for (const action of actions) {
    ok(
      !action.includes("hidden") || /data-action="reset"/.test(action),
      `${action} is either revealed with the group or stays away until editing`,
    );
  }
});

test("the output panel is hidden until a program runs", () => {
  match(renderPage({ blocks: [alcy] }).html, /<div class="code-output" hidden><\/div>/);
});

test("nothing the reader sees depends on JavaScript being on", () => {
  // Every hidden section of the page is a control, never content: a
  // <div class="code-view"> or the article itself must never be hidden.
  const page = renderPage({ blocks: [alcy, sh] });
  // A hidden element is a control a script reveals or a panel it fills.
  // Nothing that carries the page's words is ever hidden, and no hidden
  // element wraps the article.
  // The attribute, not the substring: `aria-hidden` names decoration
  // that is already invisible to a screen reader and hides no content.
  const hidden = page.html.match(/<(?!svg|button)[a-z]+[^>]*\shidden(?:\s|>)/gi) ?? [];
  ok(hidden.length > 0, "the article has controls to reveal");
  for (const element of hidden) {
    ok(
      /class="(code-actions|code-output)"/.test(element),
      `${element} hides a script-wired control rather than content`,
    );
  }
  // The article's own text is never hidden.
  ok(!/<main[^>]*hidden/.test(page.html), "the article is always shown");
});

test("a page without code loads no script beyond the shell", () => {
  const page = renderPage({ blocks: [prose] });
  equal(page.scripts.length, 1, page.scripts.join(", "));
  match(page.scripts[0] ?? "", /shared\/shell\.js$/);
});

test("the contents page is a list of links, not a script", () => {
  const html = renderTreeIndex(
    [makePage({ blocks: [prose] })],
    "en",
    "INLINE_BOOT",
    { text: "TESTSTAMP", url: "" },
    GUIDE_TREE,
  );
  ok(readPage(html).text.includes("Title"));
  match(html, /<li><a href="x\.html">Title<\/a><\/li>/);
});

test("the 404 page keeps its words and its way home", () => {
  const html = renderNotFound("INLINE_BOOT", { text: "TESTSTAMP", url: "" }, (code) => code);
  const page = readPage(html);
  ok(page.text.includes("Page not found"), "the message is in the body");
  match(html, /href="guide\/"/);
});

test("the editor is fetched on the first Edit, not with the page", () => {
  // A reader who only presses Run pays for Run: the page names the
  // module that answers it, and the editor's half arrives by import.
  const page = renderPage({ blocks: [alcy] });
  ok(page.scripts.some((src) => /shared\/guide\.js$/.test(src)), page.scripts.join(", "));
  ok(!page.scripts.some((src) => /guide-edit\.js$/.test(src)), "the page does not name the editor");
  // The entry defers it: a static import would put it back on the page.
  const entry = readFileSync("site/shared/guide.ts", "utf8");
  match(entry, /import\("\.\/guide-edit\.js"\)/);
  ok(
    !/^import \{[^}]*\} from "\.\/guide-edit\.js"/m.test(entry),
    "guide-edit is imported only on demand",
  );
});

test("a reference page without a program loads no compiler", () => {
  // The spec is prose about the language and, in its current state, one
  // signature sketch that is not a program; a page of it asks for the
  // copy button and nothing that would fetch a wasm.
  const page = renderPage({ blocks: [prose], tree: REFERENCE_TREE });
  ok(!page.scripts.some((src) => /shared\/guide\.js$/.test(src)), "no Run, no run entry");
  ok(!page.scripts.some((src) => /guide-edit\.js$/.test(src)), "no editor");
});
