// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { deepEqual, equal, throws } from "node:assert/strict";
import { test } from "node:test";

import { parseMarkdown, renderInline } from "../src/markdown.js";

test("paragraphs join, wrap, and split on blank lines", () => {
  deepEqual(parseMarkdown("one\ntwo\n\nthree\n"), [
    { type: "paragraph", text: "one two", line: 1 },
    { type: "paragraph", text: "three", line: 4 },
  ]);
});

test("headings keep their level and line", () => {
  deepEqual(parseMarkdown("# Title\n\n### Deep"), [
    { type: "heading", level: 1, text: "Title", line: 1 },
    { type: "heading", level: 3, text: "Deep", line: 3 },
  ]);
});

test("code fences carry their language and text", () => {
  const blocks = parseMarkdown("before\n\n```alcy\nfn main() {}\n```\nafter\n");
  equal(blocks.length, 3);
  const code = blocks[1];
  if (code?.type !== "code") {
    throw new Error("the second block is not a code block");
  }
  equal(code.language, "alcy");
  equal(code.text, "fn main() {}");
  equal(code.line, 3);
});

test("an unclosed fence is an error", () => {
  throws(() => parseMarkdown("```alcy\nfn main() {}\n"));
});

test("lists of both kinds, with indented continuations", () => {
  deepEqual(parseMarkdown("- one\n- two\n  continued\n\n1. first\n2. second\n"), [
    { type: "list", ordered: false, items: ["one", "two continued"], line: 1 },
    { type: "list", ordered: true, items: ["first", "second"], line: 5 },
  ]);
});

test("a blockquote joins its lines", () => {
  deepEqual(parseMarkdown("> one\n> two\n"), [
    { type: "blockquote", text: "one two", line: 1 },
  ]);
});

test("inline markup renders and text is escaped", () => {
  equal(
    renderInline("a `x < y` and **b** and *c* and <script>"),
    "a <code>x &lt; y</code> and <strong>b</strong> and <em>c</em> and &lt;script&gt;",
  );
});

test("backslash escapes a marker", () => {
  equal(renderInline("\\*not emphasis\\*"), "*not emphasis*");
});

test("links are resolved and attributes are escaped", () => {
  equal(
    renderInline("[a & b](target.md)", (url) => `${url}?x="y"`),
    '<a href="target.md?x=&quot;y&quot;">a &amp; b</a>',
  );
});

test("a link inside a label stays text", () => {
  equal(
    renderInline("[outer [inner](x.md)](y.md)"),
    '<a href="y.md">outer [inner](x.md)</a>',
  );
});
