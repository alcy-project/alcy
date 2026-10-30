// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The language reference: `docs/spec/`, published as pages. The spec is
// the language a program is written against, and it was already written
// down before this site existed; the site's job is to put it where a
// reader can read it, and to hold it to the same gates as the guide --
// every `alcy` fence compiles, every link resolves.
//
// A spec file becomes one page. Its order is the order the spec itself
// reads in, so the navigation matches the table of contents a reader
// finds in `docs/spec/overview.md`.

import { readFileSync, readdirSync } from "node:fs";
import { join, posix } from "node:path";

import { prefixOf } from "./languages.js";
import type { Language } from "./languages.js";
import { parseMarkdown } from "./markdown.js";
import type { Block } from "./markdown.js";

export interface SpecSource {
  // The path under `docs/spec/`, e.g. `types.md`.
  sourceRel: string;
  // The path inside one language tree, e.g. `reference/types.html`.
  path: string;
  order: number;
  title: string;
  blocks: Block[];
}

export interface SpecPage extends SpecSource {
  language: Language;
  route: string;
}

// The spec's reading order. A file the list does not name still gets a
// page, sorted after the named ones by name, so adding a spec file
// cannot fail the build -- it can only land in an unexpected place.
// `grammar.ebnf` is the machine-readable grammar the prose refers to;
// it is not a page.
const SPEC_ORDER = [
  "overview.md",
  "keywords.md",
  "values.md",
  "types.md",
  "items.md",
  "modules.md",
  "control.md",
  "ownership.md",
  "summaries.md",
  "ffi.md",
  "fmt.md",
  "comp.md",
  "errors.md",
  "deferred.md",
];

export function discoverSpec(specDir: string): SpecSource[] {
  const names = readdirSync(specDir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith(".md"))
    .map((entry) => entry.name)
    .sort();
  const pages: SpecSource[] = [];
  // Filled as the files are read; a mention resolves against the whole
  // set, so a file added without a place in the reading order still
  // links. The set is complete before any page renders.
  const published = new Set<string>();
  for (const name of names) {
    published.add(name);
    const blocks = linkSpecReferences(
      parseMarkdown(readFileSync(join(specDir, name), "utf8")),
      published,
    );
    const first = blocks[0];
    if (first === undefined || first.type !== "heading" || first.level !== 1) {
      throw new Error(`spec/${name}: the file must open with a level-1 heading`);
    }
    const known = SPEC_ORDER.indexOf(name);
    pages.push({
      sourceRel: name,
      path: posix.join("reference", name.replace(/\.md$/, ".html")),
      order: known === -1 ? SPEC_ORDER.length : known,
      title: first.text,
      blocks,
    });
  }
  pages.sort((a, b) => a.order - b.order || a.path.localeCompare(b.path));
  return pages;
}

// Files the spec publishes next to its pages: the normative grammar the
// prose points at. They carry no chrome and their own content type, so a
// reader can open one and a tool can fetch it.
export interface SpecAsset {
  // The path under `docs/spec/`.
  sourceRel: string;
  // The path inside one language tree.
  path: string;
}

const SPEC_ASSET = /\.(ebnf|txt)$/;

export function discoverSpecAssets(specDir: string): SpecAsset[] {
  return readdirSync(specDir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && SPEC_ASSET.test(entry.name))
    .map((entry) => ({
      sourceRel: entry.name,
      path: posix.join("reference", entry.name),
    }))
    .sort((a, b) => a.path.localeCompare(b.path));
}

// The spec names its siblings the way a document on disk does:
// `types.md` in backticks. A reader on a web page has no directory to
// look in, so a mention of a spec file that has a page becomes a link
// to it. Only names with a page are rewritten; a mention of a file
// that is not published stays text.
const SPEC_REFERENCE = /`([a-z0-9-]+\.md)`/g;

export function linkSpecReferences(
  blocks: readonly Block[],
  published: ReadonlySet<string> = new Set(SPEC_ORDER),
): Block[] {
  const linkNames = (text: string): string =>
    text.replace(SPEC_REFERENCE, (whole, name: string) => {
      const slug = name.slice(0, -3);
      return published.has(`${slug}.md`) ? `[\`${name}\`](${slug}.html)` : whole;
    });

  return blocks.map((block) => {
    if (block.type === "list") {
      const items = block.items.map(linkNames);
      return items.every((item, index) => item === block.items[index])
        ? block
        : { ...block, items };
    }
    if (block.type === "code") {
      return block;
    }
    const text = linkNames(block.text);
    return text === block.text ? block : { ...block, text };
  });
}

export function specPagesForLanguage(
  sources: readonly SpecSource[],
  language: Language,
): SpecPage[] {
  return sources.map((source) => ({
    ...source,
    language,
    route: prefixOf(language) + source.path,
  }));
}
