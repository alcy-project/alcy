// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The one header every page stamps, and the placeholder the
// landing and playground templates share to stamp it.

import { equal, match, ok } from "node:assert/strict";
import { test } from "node:test";

import { siteHeader } from "../src/render.js";
import { stampTemplate } from "../src/templates.js";

test("the header names all five destinations", () => {
  const html = siteHeader({
    route: "index.html",
    language: "en",
    treePath: "index.html",
    currentSegment: null,
  });
  match(html, /<nav class="site-nav" aria-label="Site">/);
  for (const [href, label] of [
    ["./guide/", "Guide"],
    ["./reference/", "Reference"],
    ["./tools/", "Tools"],
    ["./playground/", "Playground"],
  ]) {
    ok(html.includes(`<a href="${href}"`), `missing ${href}`);
    ok(html.includes(`>${label}<`), `missing ${label}`);
  }
  ok(html.includes('data-action="search"'));
  ok(!html.includes("aria-current"), "the landing names no current tree");
});

test("a tree page marks its own tree current", () => {
  const html = siteHeader({
    route: "reference/ownership.html",
    language: "en",
    treePath: "reference/ownership.html",
    currentSegment: "reference",
  });
  ok(html.includes('<a href="../reference/" aria-current="page">Reference</a>'));
  ok(html.includes('<a href="../tools/">Tools</a>'));
});

test("the playground marks itself current", () => {
  const html = siteHeader({
    route: "playground/index.html",
    language: "en",
    treePath: "playground/index.html",
    currentSegment: "playground",
  });
  ok(html.includes('<a href="../playground/" aria-current="page">Playground</a>'));
  ok(html.includes('<a href="../reference/">Reference</a>'));
  ok(html.includes('<a href="../tools/">Tools</a>'));
});

test("the Japanese header stamps Japanese labels", () => {
  const html = siteHeader({
    route: "ja/index.html",
    language: "ja",
    treePath: "index.html",
    currentSegment: null,
  });
  for (const label of ["ガイド", "リファレンス", "ツール", "プレイグラウンド", "検索"]) {
    ok(html.includes(`>${label}<`), `missing ${label}`);
  }
});

test("the template's header placeholder stamps the computed header", () => {
  const header = siteHeader({
    route: "index.html",
    language: "en",
    treePath: "index.html",
    currentSegment: null,
  });
  equal(stampTemplate("{{header}}", "en", { header }), header);
});
