// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's pages: which source files there are, in what order they
// read, and what each one's path is. A source is parsed once; the
// generator derives one page per language from it, because only the
// chrome around the article differs.

import { readFileSync, readdirSync } from "node:fs";
import { join, posix } from "node:path";

import { prefixOf } from "./languages.js";
import type { Language } from "./languages.js";
import { parseMarkdown } from "./markdown.js";
import type { Block } from "./markdown.js";

export interface GuideSource {
  // The path under `docs/guide/`, e.g. `01-introduction.md`.
  sourceRel: string;
  // The path inside one language tree, e.g. `guide/introduction.html`.
  path: string;
  order: number;
  title: string;
  blocks: Block[];
}

export interface GuidePage extends GuideSource {
  language: Language;
  // The path under the built site's root, e.g. `ja/guide/introduction.html`.
  route: string;
}

// `NN-slug.md`: the number orders the pages, the slug is the URL.
const GUIDE_FILE = /^(\d+)-([a-z0-9-]+)\.md$/;

function walk(dir: string, prefix: string): string[] {
  const found: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const relative = prefix === "" ? entry.name : posix.join(prefix, entry.name);
    if (entry.isDirectory()) {
      found.push(...walk(join(dir, entry.name), relative));
    } else if (entry.isFile() && entry.name.endsWith(".md")) {
      found.push(relative);
    }
  }
  return found;
}

export function discoverGuide(docsDir: string): GuideSource[] {
  const sources = walk(docsDir, "").sort();
  const pages: GuideSource[] = [];
  const orders = new Map<number, string>();

  for (const sourceRel of sources) {
    const match = GUIDE_FILE.exec(posix.basename(sourceRel));
    if (match === null) {
      throw new Error(`${sourceRel}: guide files are named NN-slug.md`);
    }
    const order = Number(match[1]);
    const previous = orders.get(order);
    if (previous !== undefined) {
      throw new Error(`${sourceRel}: the number ${order} is already used by ${previous}`);
    }
    orders.set(order, sourceRel);

    const blocks = parseMarkdown(readFileSync(join(docsDir, sourceRel), "utf8"));
    const first = blocks[0];
    if (first === undefined || first.type !== "heading" || first.level !== 1) {
      throw new Error(`${sourceRel}: the file must open with a level-1 heading`);
    }
    const dir = posix.dirname(sourceRel);
    const name = `${match[2] ?? ""}.html`;
    const path = dir === "." ? posix.join("guide", name) : posix.join("guide", dir, name);
    pages.push({ sourceRel, path, order, title: first.text, blocks });
  }

  pages.sort((a, b) => a.order - b.order || a.path.localeCompare(b.path));
  return pages;
}

export function pagesForLanguage(
  sources: readonly GuideSource[],
  language: Language,
): GuidePage[] {
  return sources.map((source) => ({
    ...source,
    language,
    route: prefixOf(language) + source.path,
  }));
}
