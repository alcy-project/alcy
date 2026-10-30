// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { equal, match, ok } from "node:assert/strict";
import { test } from "node:test";

import type { GuidePage } from "../src/guide.js";
import type { Block } from "../src/markdown.js";
import {
  GUIDE_TREE,
  REFERENCE_TREE,
  renderGuidePage,
  renderNotFound,
} from "../src/render.js";
import { canonicalUrl } from "../src/urls.js";

function page(language: "en" | "ja", blocks: Block[]): GuidePage {
  return {
    sourceRel: "01-x.md",
    path: "guide/x.html",
    route: language === "en" ? "guide/x.html" : "ja/guide/x.html",
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

const sh: Block = { type: "code", language: "sh", text: "echo hi", line: 1 };
const alcy: Block = { type: "code", language: "alcy", text: "fn main() {}", line: 1 };
const prose: Block = { type: "paragraph", text: "hello", line: 1 };

test("sections gain anchors and an outline beside the article", () => {
  const html = render("en", [
    { type: "heading", level: 1, text: "Title", line: 1 },
    { type: "heading", level: 2, text: "Values", line: 3 },
    prose,
    { type: "heading", level: 2, text: "Functions", line: 5 },
    prose,
  ]);
  match(html, /<h1>Title<\/h1>/);
  match(html, /<h2 id="values">Values<\/h2>/);
  match(html, /<h2 id="functions">Functions<\/h2>/);
  match(html, /<div class="guide-aside">/);
  match(html, /<details class="guide-outline">/);
  match(html, /<summary>On this page<\/summary>/);
  match(html, /<li><a href="#functions">Functions<\/a><\/li>/);
});

test("only a runnable block loads the compiler's half of the guide", () => {
  const examples = render("en", [prose, sh]);
  match(examples, /<script type="module" src="\.\.\/shared\/guide-copy.js"><\/script>/);
  equal(examples.includes("guide.js"), false);
  equal(examples.includes("../shared/guide.js\" />"), false);
  const runnable = render("en", [alcy]);
  match(runnable, /<script type="module" src="\.\.\/shared\/guide-copy.js"><\/script>/);
  match(runnable, /<script type="module" src="\.\.\/shared\/guide.js"><\/script>/);
});

test("the outline says how much each section asks of the reader", () => {
  // The count is decided at build time, from the fences the section
  // actually holds: a reader scanning the outline learns that one section
  // holds three programs before they scroll to it.
  const html = render("en", [
    { type: "heading", level: 1, text: "Title", line: 1 },
    { type: "heading", level: 2, text: "Values", line: 3 },
    alcy,
    alcy,
    { type: "heading", level: 2, text: "Functions", line: 7 },
    alcy,
    { type: "heading", level: 2, text: "Notes", line: 11 },
    prose,
  ]);
  match(html, /<li><a href="#values">Values <span class="outline-tally">2 programs<\/span><\/a><\/li>/);
  match(html, /<li><a href="#functions">Functions <span class="outline-tally">1 program<\/span><\/a><\/li>/);
  // A section with nothing to run says nothing: the absence is the message.
  match(html, /<li><a href="#notes">Notes<\/a><\/li>/);
});

test("the outline counts only the fences that run", () => {
  // A shell block is something to copy, not something to run, and a
  // level-3 heading does not start a new section.
  const html = render("en", [
    { type: "heading", level: 1, text: "Title", line: 1 },
    { type: "heading", level: 2, text: "Values", line: 3 },
    sh,
    { type: "heading", level: 3, text: "Detail", line: 5 },
    alcy,
    { type: "heading", level: 2, text: "Functions", line: 9 },
    prose,
  ]);
  match(html, /<li><a href="#values">Values <span class="outline-tally">1 program<\/span><\/a><\/li>/);
  match(html, /<li><a href="#functions">Functions<\/a><\/li>/);
});

test("a page without sections carries no outline", () => {
  const html = render("en", [prose]);
  equal(html.includes("guide-outline"), false);
});

test("the footer carries the build's stamp, linked to its source", () => {
  const html = render("en", [prose]);
  match(
    html,
    /<span>alcy · <a href="https:\/\/example\.test\/commit\/TESTSTAMP" rel="noopener">TESTSTAMP<\/a><\/span>/,
  );
});

test("the theme bootstrap rides inline in the head", () => {
  const html = render("en", [prose]);
  match(html, /<script>INLINE_BOOT<\/script>/);
  equal(html.includes("boot.js"), false);
});

test("a code block gains a copy button, hidden until a script reveals it", () => {
  const html = render("en", [sh]);
  match(html, /<div class="code-block">/);
  match(html, /<div class="code-actions" hidden>/);
  match(html, /data-action="copy">Copy</);
});

test("the copy label is stamped in the page's language", () => {
  match(render("ja", [sh]), /data-action="copy">コピー</);
});

test("the guide's assets load only when a page has code", () => {
  const withCode = render("en", [alcy]);
  match(withCode, /shared\/guide\.css/);
  match(withCode, /shared\/guide\.js/);
  equal(render("en", [prose]).includes("shared/guide."), false);
});

test("an alcy block gains edit, run, and an output panel", () => {
  const html = render("en", [alcy]);
  match(html, /<div class="code-block" data-language="alcy">/);
  match(html, /<div class="code-view">\s*<pre><code>/);
  match(html, /data-action="edit" aria-pressed="false">Edit</);
  match(html, /data-action="reset" hidden>Reset</);
  match(html, /data-action="run">Run</);
  match(html, /data-action="open" title="Open this program in the playground">Open</);
  match(html, /<div class="code-output" hidden><\/div>/);
});

test("a non-alcy block stays copy-only", () => {
  const html = render("en", [sh]);
  equal(html.includes('data-action="edit"'), false);
  equal(html.includes('data-action="run"'), false);
  equal(html.includes("code-output"), false);
  equal(html.includes("data-language"), false);
});

test("the alcy labels are stamped in the page's language", () => {
  const html = render("ja", [alcy]);
  match(html, /data-action="edit" aria-pressed="false">編集</);
  match(html, /data-action="run">実行</);
});

test("a page names its canonical and alternate addresses", () => {
  const html = render("en", [prose]);
  match(
    html,
    /<link rel="canonical" href="https:\/\/alcy-project\.github\.io\/alcy\/guide\/x\.html" \/>/,
  );
  match(
    html,
    /<link rel="alternate" hreflang="ja" href="https:\/\/alcy-project\.github\.io\/alcy\/ja\/guide\/x\.html" \/>/,
  );
  match(html, /<link rel="icon" href="\.\.\/favicon\.svg" \/>/);
});

test("an index route is named at its directory", () => {
  equal(canonicalUrl("index.html"), "https://alcy-project.github.io/alcy/");
  equal(
    canonicalUrl("ja/guide/index.html"),
    "https://alcy-project.github.io/alcy/ja/guide/",
  );
  equal(
    canonicalUrl("guide/x.html"),
    "https://alcy-project.github.io/alcy/guide/x.html",
  );
});

test("the 404 page keeps its links on the deployment root", () => {
  const html = renderNotFound("INLINE_BOOT", { text: "TESTSTAMP", url: "" }, (code) => code);
  match(html, /<script>INLINE_BOOT<\/script>/);
  equal(html.includes("boot.js"), false);
  match(html, /<span>alcy · TESTSTAMP<\/span>/);
  // The playful block: runnable like the guide's but read-only -- the
  // page's own links already reach the playground.
  match(html, /<link rel="stylesheet" href="shared\/guide.css" \/>/);
  match(html, /<script type="module" src="shared\/guide-copy.js"><\/script>/);
  match(html, /<script type="module" src="shared\/guide.js"><\/script>/);
  match(html, /<div class="code-block" data-language="alcy">/);
  match(html, /println\("Page not found"\)/);
  match(html, /ret 404/);
  match(html, /data-action="copy">Copy</);
  match(html, /data-action="run">Run</);
  equal(html.includes('data-action="edit"'), false);
  equal(html.includes('data-action="reset"'), false);
  equal(html.includes('data-action="open"'), false);
  match(html, /name="robots" content="noindex"/);
  match(html, /location\.pathname\.startsWith\("\/alcy\/"\)/);
  match(html, /href="guide\/"/);
});
test("the guide's last page continues into the reference", () => {
  const first = page("en", [prose]);
  const last = { ...page("en", [prose]), path: "guide/z.html", route: "guide/z.html", title: "Last" };
  const html = renderGuidePage(last, {
    pages: [first, last],
    tree: GUIDE_TREE,
    highlight: (code) => code,
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "" },
  });
  match(html, /<a class="pager-onward" href="\.\.\/reference\/index\.html">Next · Reference/);
});

test("the reference does not send a reader back into the guide", () => {
  const target = page("en", [prose]);
  const html = renderGuidePage(target, {
    pages: [target],
    tree: REFERENCE_TREE,
    highlight: (code) => code,
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "" },
  });
  ok(!/pager-onward/.test(html));
});

