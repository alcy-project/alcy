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

import type { GuidePage } from "../src/guide.js";
import type { Block } from "../src/markdown.js";
import {
  GUIDE_TREE,
  REFERENCE_TREE,
  renderGuidePage,
  renderNotFound,
  renderTreeIndex,
} from "../src/render.js";

function page(blocks: Block[]): GuidePage {
  return {
    sourceRel: "01-x.md",
    path: "guide/x.html",
    route: "guide/x.html",
    language: "en",
    order: 1,
    title: "Title",
    blocks,
  };
}

function render(blocks: Block[]): string {
  const target = page(blocks);
  return renderGuidePage(target, {
    pages: [target],
    tree: GUIDE_TREE,
    highlight: (code) => `<span class="hl-keyword">${code}</span>`,
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "https://example.test/commit/TESTSTAMP" },
  });
}

// The page's visible text: the article and its surrounding furniture,
// with the head's script and the scripts at the end of the body dropped.
function textOutsideScripts(html: string): string {
  const body = html.slice(html.indexOf("<body"));
  return body
    .replace(/<script[\s\S]*?<\/script>/g, "")
    .replace(/<style[\s\S]*?<\/style>/g, "")
    .replace(/<[^>]+>/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

const prose: Block = { type: "paragraph", text: "hello reader", line: 1 };
const alcy: Block = { type: "code", language: "alcy", text: "fn main() {}", line: 1 };
const sh: Block = { type: "code", language: "sh", text: "alcy run main.al", line: 3 };

test("the article's prose is in the document, not assembled by a script", () => {
  const html = render([
    { type: "heading", level: 1, text: "Title", line: 1 },
    prose,
  ]);
  const text = textOutsideScripts(html);
  ok(text.includes("hello reader"), "the paragraph text is in the body");
  ok(text.includes("Title"), "the heading text is in the body");
});

test("code is highlighted into the markup the reader receives", () => {
  const html = render([alcy]);
  match(html, /<pre><code><span class="hl-keyword">fn main\(\) \{\}<\/span><\/code><\/pre>/);
});

test("a code block's text survives with its buttons hidden", () => {
  const html = render([alcy]);
  ok(textOutsideScripts(html).includes("fn main() {}"), "the program is readable");
  // Every control that needs a script carries `hidden` in the markup, so
  // a reader without one never sees a button that does nothing.
  match(html, /<div class="code-actions" hidden>/);
  // Only the block's own controls: the header's search button is the
  // page's furniture and needs no script to open its place in the page.
  // The buttons sit inside the group the markup hides; a button that is
  // itself hidden is one a script reveals later (Reset, once editing).
  const article = html.slice(html.indexOf('class="guide-article"'));
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
  match(render([alcy]), /<div class="code-output" hidden><\/div>/);
});

test("nothing the reader sees depends on JavaScript being on", () => {
  // Every hidden section of the page is a control, never content: a
  // <div class="code-view"> or the article itself must never be hidden.
  const html = render([alcy, sh]);
  const article = html.slice(html.indexOf('class="guide-article"'));
  // A hidden element is a control a script reveals or a panel it fills.
  // Nothing that carries the page's words is ever hidden, and no hidden
  // element wraps the article.
  const hidden = html.match(/<(?!svg|button)[a-z]+[^>]*\bhidden\b[^>]*>/gi) ?? [];
  ok(hidden.length > 0, "the article has controls to reveal");
  for (const element of hidden) {
    ok(
      /class="(code-actions|code-output)"/.test(element),
      `${element} hides a script-wired control rather than content`,
    );
  }
  // The article's own text is never hidden.
  ok(!/<main[^>]*hidden/.test(article), "the article is always shown");
});

test("a page without code loads no script beyond the shell", () => {
  const html = render([prose]);
  const scripts = html.match(/<script TYPE="module"[^>]*src="[^"]+"/gi) ?? [];
  equal(scripts.length, 1);
  match(html, /src="\.\.\/shared\/shell\.js"/);
});

test("the contents page is a list of links, not a script", () => {
  const html = renderTreeIndex(
    [page([prose])],
    "en",
    "INLINE_BOOT",
    { text: "TESTSTAMP", url: "" },
    GUIDE_TREE,
  );
  ok(textOutsideScripts(html).includes("Title"));
  match(html, /<li><a href="x\.html">Title<\/a><\/li>/);
});

test("the 404 page keeps its words and its way home", () => {
  const html = renderNotFound("INLINE_BOOT", { text: "TESTSTAMP", url: "" }, (code) => code);
  const text = textOutsideScripts(html);
  ok(text.includes("Page not found"), "the message is in the body");
  match(html, /href="guide\/"/);
});

test("the editor is fetched on the first Edit, not with the page", () => {
  // A reader who only presses Run pays for Run: the page names the
  // module that answers it, and the editor's half arrives by import.
  const html = render([alcy]);
  match(html, /src="[^"]*shared\/guide\.js"/);
  ok(!/src="[^"]*guide-edit\.js"/.test(html), "the page does not name the editor");
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
  const target = page([prose]);
  const html = renderGuidePage(target, {
    pages: [target],
    tree: REFERENCE_TREE,
    highlight: (code) => code,
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "" },
  });
  ok(!/shared\/guide\.js/.test(html), "no Run, no run entry");
  ok(!/guide-edit\.js/.test(html), "no editor");
});

