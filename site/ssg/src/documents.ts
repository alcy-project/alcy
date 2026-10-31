// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// What a source document is, and what one language's page for it is.
// The guide and the reference publish different sets of files found in
// different ways -- a filename convention against an explicit reading
// order -- but everything after the discovery is the same: a source
// becomes one page per language, and only the chrome differs.

import { readFileSync, readdirSync } from "node:fs";
import { join, posix } from "node:path";

import { prefixOf } from "./languages.js";
import type { Language } from "./languages.js";
import type { Block } from "./markdown.js";

export interface DocSource {
  // The path under the documentation directory, e.g. `types.md`.
  sourceRel: string;
  // The path inside one language tree, e.g. `reference/types.html`.
  path: string;
  order: number;
  title: string;
  blocks: Block[];
}

export interface DocPage extends DocSource {
  language: Language;
  // The path under the built site's root, e.g. `ja/reference/types.html`.
  route: string;
}

// Every `.md` file under `dir`, as paths relative to it, sorted so a
// discovery that does not order its own output is still deterministic.
export function markdownFiles(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith(".md"))
    .map((entry) => entry.name)
    .sort();
}

// Both trees read the same way: the file opens with the heading that
// names it, so a page's title is its source's first line rather than a
// second place to keep it in step.
export function titleOf(blocks: readonly Block[], sourceRel: string): string {
  const first = blocks[0];
  if (first === undefined || first.type !== "heading" || first.level !== 1) {
    throw new Error(`${sourceRel}: the file must open with a level-1 heading`);
  }
  return first.text;
}

export function pagesForLanguage(
  sources: readonly DocSource[],
  language: Language,
): DocPage[] {
  return sources.map((source) => ({
    ...source,
    language,
    route: prefixOf(language) + source.path,
  }));
}
