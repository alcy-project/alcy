// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { equal, match } from "node:assert/strict";
import { test } from "node:test";

import { linkSpecReferences } from "../src/spec.js";
import type { Block } from "../src/markdown.js";

function paragraph(text: string): Block {
  return { type: "paragraph", text, line: 1 };
}

test("a spec file a page links to becomes reachable", () => {
  const [block] = linkSpecReferences([paragraph("see `types.md` for the tower")]);
  equal(block?.type, "paragraph");
  match((block as { text: string }).text, /\[`types\.md`\]\(types\.html\)/);
});

test("a list's items are linked too", () => {
  const [block] = linkSpecReferences([
    { type: "list", ordered: false, items: ["see `items.md`"], line: 1 },
  ]);
  const items = (block as { items: string[] }).items;
  equal(items[0], "see [`items.md`](items.html)");
});

test("a name with no page stays text", () => {
  const [block] = linkSpecReferences([paragraph("write `notes.md` if you like")]);
  equal((block as { text: string }).text, "write `notes.md` if you like");
});

test("a fence's text is left alone", () => {
  const code: Block = { type: "code", language: "text", text: "`types.md`", line: 1 };
  equal(linkSpecReferences([code])[0], code);
});
