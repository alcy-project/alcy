// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The command reference: `docs/tools/`, published as pages. The command
// line is the toolchain; this tree is where a reader finds what every
// verb takes and reports, which flags the shared command exposes, and
// what each exit code means.
//
// A file becomes one page. The order is editorial: the canonical entry
// point first, then build verbs, then test and fmt/lint/lsp, then the
// scaffolding verbs. A file the list does not name lands last, sorted
// by name, so adding one cannot break the build.
import { readFileSync, readdirSync } from "node:fs";
import { join, posix } from "node:path";

import { markdownFiles, titleOf } from "./documents.js";
import type { DocPage, DocSource } from "./documents.js";
import { parseMarkdown } from "./markdown.js";
import type { Block } from "./markdown.js";

export type ToolsSource = DocSource;
export type ToolsPage = DocPage;

const TOOLS_ORDER = [
  "index.md",
  "build.md",
  "compile.md",
  "check.md",
  "test.md",
  "run.md",
  "fmt.md",
  "lint.md",
  "lsp.md",
  "new.md",
  "init.md",
];

export function discoverTools(toolsDir: string): ToolsSource[] {
  const names = markdownFiles(toolsDir);
  const pages: ToolsSource[] = [];
  // Every file is published, so a mention resolves against the whole
  // directory.
  const published = new Set(names);
  for (const name of names) {
    const blocks = linkToolsReferences(
      parseMarkdown(readFileSync(join(toolsDir, name), "utf8")),
      published,
    );
    const known = TOOLS_ORDER.indexOf(name);
    pages.push({
      sourceRel: name,
      path: posix.join("tools", name.replace(/\.md$/, ".html")),
      order: known === -1 ? TOOLS_ORDER.length : known,
      title: titleOf(blocks, `tools/${name}`),
      blocks,
    });
  }
  pages.sort((a, b) => a.order - b.order || a.path.localeCompare(b.path));
  return pages;
}

// The tools tree names its siblings the way a directory does: the
// sibling pages, referenced as `build.md`. A reader on a web page has
// no directory to look in, so a mention of a page that has been
// published becomes a link to it. A mention that is not a page is left
// as text.
const TOOLS_REFERENCE = /`([a-z0-9-]+\.md)`/g;

export function linkToolsReferences(
  blocks: readonly Block[],
  published: ReadonlySet<string> = new Set(TOOLS_ORDER),
): Block[] {
  const linkNames = (text: string): string =>
    text.replace(TOOLS_REFERENCE, (whole, name: string) => {
      return published.has(name) ? `[\`${name}\`](${name.slice(0, -3)}.html)` : whole;
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