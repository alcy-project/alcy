// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { deepEqual, equal } from "node:assert/strict";
import { test } from "node:test";

import type { GuidePage } from "../src/guide.js";
import { createLinkChecker } from "../src/links.js";

function page(sourceRel: string, route: string): GuidePage {
  return { sourceRel, route, order: 0, title: sourceRel, blocks: [] };
}

const pages = [
  page("01-introduction.md", "guide/introduction.html"),
  page("02-getting-started.md", "guide/getting-started.html"),
  page("sub/03-more.md", "guide/sub/more.html"),
];

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

test("a page link becomes the built html", () => {
  const links = checker({});
  equal(
    links.resolve("02-getting-started.md", pages[0] as GuidePage),
    "getting-started.html",
  );
  equal(links.resolve("sub/03-more.md", pages[0] as GuidePage), "sub/more.html");
  equal(
    links.resolve("../01-introduction.md#values", pages[2] as GuidePage),
    "../introduction.html#values",
  );
  deepEqual(links.errors, []);
});

test("a repository markdown link becomes a GitHub URL", () => {
  const links = checker({ repo: ["docs/spec/01-lexical.md", "docs/adr/0057-x.md"] });
  equal(
    links.resolve("../spec/01-lexical.md", pages[0] as GuidePage),
    "https://github.com/alcy-project/alcy/blob/main/docs/spec/01-lexical.md",
  );
  equal(
    links.resolve("../adr/0057-x.md#decision", pages[1] as GuidePage),
    "https://github.com/alcy-project/alcy/blob/main/docs/adr/0057-x.md#decision",
  );
  deepEqual(links.errors, []);
});

test("a missing repository file is an error", () => {
  const links = checker({});
  equal(links.resolve("../spec/99-nope.md", pages[0] as GuidePage), "../spec/99-nope.md");
  equal(links.errors.length, 1);
});

test("a guide link that names no page is an error", () => {
  const links = checker({});
  links.resolve("99-nope.md", pages[0] as GuidePage);
  equal(links.errors.length, 1);
});

test("site links are checked, with directories and the root", () => {
  const links = checker({ site: ["playground/", "guide/", "guide/introduction.html"] });
  const intro = pages[0] as GuidePage;
  equal(links.resolve("../playground/", intro), "../playground/");
  equal(links.resolve("introduction.html", intro), "introduction.html");
  equal(links.resolve("../", intro), "../");
  deepEqual(links.errors, []);
});

test("a site-root link is rebased onto the page", () => {
  const links = checker({ site: ["", "playground/", "guide/sub/more.html"] });
  const intro = pages[0] as GuidePage;
  const more = pages[2] as GuidePage;
  equal(links.resolve("/playground/", intro), "../playground/");
  equal(links.resolve("/", intro), "../");
  equal(links.resolve("/playground/", more), "../../playground/");
  equal(links.resolve("/guide/sub/more.html#x", intro), "sub/more.html#x");
  deepEqual(links.errors, []);
});

test("a missing site target is an error", () => {
  const links = checker({});
  equal(links.resolve("../playground/", pages[0] as GuidePage), "../playground/");
  equal(links.errors.length, 1);
});

test("external, anchor, and mailto links pass through", () => {
  const links = checker({});
  const intro = pages[0] as GuidePage;
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
