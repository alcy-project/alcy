// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { deepEqual, equal } from "node:assert/strict";
import { test } from "node:test";

import type { GuidePage } from "../src/guide.js";
import type { Language } from "../src/languages.js";
import { prefixOf } from "../src/languages.js";
import { createLinkChecker } from "../src/links.js";

function page(sourceRel: string, path: string, language: Language = "en"): GuidePage {
  return {
    sourceRel,
    path,
    route: prefixOf(language) + path,
    language,
    order: 0,
    title: sourceRel,
    blocks: [],
  };
}

const intro = page("01-introduction.md", "guide/introduction.html");
const gettingStarted = page("02-getting-started.md", "guide/getting-started.html");
const more = page("sub/03-more.md", "guide/sub/more.html");
const jaIntro = page("01-introduction.md", "guide/introduction.html", "ja");
const jaGettingStarted = page("02-getting-started.md", "guide/getting-started.html", "ja");
const jaMore = page("sub/03-more.md", "guide/sub/more.html", "ja");
const pages = [intro, gettingStarted, more, jaIntro, jaGettingStarted, jaMore];

function checker(options: {
  site?: string[];
  repo?: string[];
}) {
  const site = new Set(options.site ?? []);
  const repo = new Set(options.repo ?? []);
  return createLinkChecker({
    pages,
    siteExists: (route) => site.has(route) || route === "",
    repoExists: (path) => repo.has(path),
  });
}

test("a page link becomes the built html in the same tree", () => {
  const links = checker({});
  equal(links.resolve("02-getting-started.md", intro), "getting-started.html");
  equal(links.resolve("sub/03-more.md", intro), "sub/more.html");
  equal(links.resolve("../01-introduction.md#values", more), "../introduction.html#values");
  equal(links.resolve("02-getting-started.md", jaIntro), "getting-started.html");
  deepEqual(links.errors, []);
});

test("a repository markdown link becomes a GitHub URL", () => {
  const links = checker({ repo: ["docs/spec/01-lexical.md", "docs/adr/0057-x.md"] });
  equal(
    links.resolve("../spec/01-lexical.md", intro),
    "https://github.com/alcy-project/alcy/blob/main/docs/spec/01-lexical.md",
  );
  equal(
    links.resolve("../adr/0057-x.md#decision", gettingStarted),
    "https://github.com/alcy-project/alcy/blob/main/docs/adr/0057-x.md#decision",
  );
  deepEqual(links.errors, []);
});

test("a missing repository file is an error", () => {
  const links = checker({});
  equal(links.resolve("../spec/99-nope.md", intro), "../spec/99-nope.md");
  equal(links.errors.length, 1);
});

test("a guide link that names no page is an error", () => {
  const links = checker({});
  links.resolve("99-nope.md", intro);
  equal(links.errors.length, 1);
});

test("site links are checked, with directories and the root", () => {
  const links = checker({ site: ["playground/", "ja/playground/", "guide/", "guide/introduction.html"] });
  equal(links.resolve("../playground/", intro), "../playground/");
  equal(links.resolve("introduction.html", intro), "introduction.html");
  equal(links.resolve("../", intro), "../");
  equal(links.resolve("../ja/playground/", intro), "../ja/playground/");
  deepEqual(links.errors, []);
});

test("a site-root link is rebased onto the tree and the page", () => {
  const links = checker({ site: ["", "playground/", "ja/playground/", "ja/guide/sub/more.html"] });
  equal(links.resolve("/playground/", intro), "../playground/");
  equal(links.resolve("/", intro), "../");
  equal(links.resolve("/playground/", more), "../../playground/");
  // In the Japanese tree the same link stays inside that tree.
  equal(links.resolve("/playground/", jaIntro), "../playground/");
  equal(links.resolve("/playground/", jaMore), "../../playground/");
  equal(links.resolve("/guide/sub/more.html#x", jaIntro), "sub/more.html#x");
  deepEqual(links.errors, []);
});

test("a missing site target is an error", () => {
  const links = checker({});
  equal(links.resolve("../playground/", intro), "../playground/");
  equal(links.errors.length, 1);
});

test("external, anchor, and mailto links pass through", () => {
  const links = checker({});
  for (const url of [
    "https://example.com/x",
    "//example.com/x",
    "#heading",
    "mailto:a@example.com",
  ]) {
    equal(links.resolve(url, intro), url);
  }
  deepEqual(links.errors, []);
});
