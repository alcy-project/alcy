// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { match } from "node:assert/strict";
import { test } from "node:test";

import { renderSettings } from "../src/shell.js";

test("the settings menu carries the theme toggle and the language link", () => {
  const html = renderSettings({ language: "ja", switchHref: "../guide/x.html" });
  match(html, /id="theme"/);
  match(html, /data-theme-mode="auto"/);
  match(html, /href="\.\.\/guide\/x\.html"/);
  match(html, /English/);
  match(html, /aria-label="設定"/);
});

test("the menu's icon is stroked, so it is visible", () => {
  // A path of lines with no paint renders nothing; the icon must carry
  // its own stroke, not rely on a stylesheet.
  const html = renderSettings({ language: "en", switchHref: "" });
  match(html, /<summary[^>]*><svg[^>]*stroke="currentColor"/);
});
