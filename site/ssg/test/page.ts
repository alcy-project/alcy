// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// One description of a rendered page, for the tests that hold the
// page's contract: what it reads as without JavaScript, what a
// screen reader announces, and what it names at the end of its body.
// Each test asks this description rather than matching the markup
// itself, so a change to the template is answered in one place.

import type { GuidePage } from "../src/guide.js";
import type { Block } from "../src/markdown.js";
import type { Tree } from "../src/render.js";
import { GUIDE_TREE, renderGuidePage } from "../src/render.js";

export interface PageSpec {
  language?: "en" | "ja";
  blocks: Block[];
  path?: string;
  tree?: Tree;
  highlight?: (code: string) => string;
  // The pages this one sits among, when the pager matters. Defaults to
  // the page alone.
  pages?: readonly GuidePage[];
}

export function makePage(spec: PageSpec): GuidePage {
  const language = spec.language ?? "en";
  const path = spec.path ?? "guide/x.html";
  return {
    sourceRel: "01-x.md",
    path,
    route: language === "en" ? path : `${language}/${path}`,
    language,
    order: 1,
    title: "Title",
    blocks: spec.blocks,
  };
}

// A rendered page, read back as the handful of properties the tests
// assert. `html` stays available for the odd exact match.
export interface RenderedPage {
  html: string;
  // The visible text: the article and its furniture, with the head's
  // script and the scripts at the end of the body dropped.
  text: string;
  // Every module the page names, by `src`.
  scripts: string[];
  // Every landmark in document order, with the label it carries, if any.
  landmarks: Landmark[];
}

export interface Landmark {
  tag: string;
  // The `aria-label` the region announces, or "" when it has none.
  name: string;
}

// The name of every nav, in document order; two regions called "Guide"
// would be told apart by position alone.
export function navNames(page: RenderedPage): string[] {
  return page.landmarks
    .filter((landmark) => landmark.tag === "nav")
    .map((landmark) => landmark.name);
}

export function readPage(html: string): RenderedPage {
  const body = html.slice(html.indexOf("<body"));
  return {
    html,
    text: body
      .replace(/<script[\s\S]*?<\/script>/g, "")
      .replace(/<style[\s\S]*?<\/style>/g, "")
      .replace(/<[^>]+>/g, " ")
      .replace(/\s+/g, " ")
      .trim(),
    scripts: Array.from(
      html.matchAll(/<script[^>]*\ssrc="([^"]+)"/gi),
      (match) => match[1] ?? "",
    ),
    landmarks: Array.from(
      html.matchAll(/<(header|nav|main|aside|footer)\b([^>]*)>/g),
      (match): Landmark => {
        const name = /aria-label="([^"]*)"/.exec(match[2] ?? "");
        return { tag: match[1] ?? "", name: name ? (name[1] ?? "") : "" };
      },
    ),
  };
}

export function renderPage(spec: PageSpec = { blocks: [] }): RenderedPage {
  const target = makePage(spec);
  const html = renderGuidePage(target, {
    pages: spec.pages ?? [target],
    tree: spec.tree ?? GUIDE_TREE,
    highlight: spec.highlight ?? ((code) => code),
    resolveLink: (url) => url,
    boot: "INLINE_BOOT",
    stamp: { text: "TESTSTAMP", url: "https://example.test/commit/TESTSTAMP" },
  });
  return readPage(html);
}
