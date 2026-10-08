// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { equal, match } from "node:assert/strict";
import { test } from "node:test";

import type { GuidePage } from "../src/guide.js";
import type { Block } from "../src/markdown.js";
import { renderGuidePage } from "../src/render.js";

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
    highlight: (code) => code,
    resolveLink: (url) => url,
  });
}

const sh: Block = { type: "code", language: "sh", text: "echo hi", line: 1 };
const alcy: Block = { type: "code", language: "alcy", text: "fn main() {}", line: 1 };
const prose: Block = { type: "paragraph", text: "hello", line: 1 };

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
  match(html, /data-action="edit" aria-pressed="false">Edit</);
  match(html, /data-action="reset" hidden>Reset</);
  match(html, /data-action="run">Run</);
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
