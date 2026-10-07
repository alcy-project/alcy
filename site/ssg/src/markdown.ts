// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// A parser for the Markdown subset the guide is written in. The subset is
// deliberately small and documented here, because it is what the guide's
// authors can rely on:
//
//   - paragraphs, wrapped over any number of lines and joined with a space
//   - ATX headings `#` through `######`
//   - fenced code blocks with an info string; `alcy` fences are complete
//     programs and are compiled at build time
//   - unordered and ordered lists, one level; an indented continuation
//     line belongs to the item above it
//   - blockquotes, one paragraph per quote
//   - inline `code`, **strong**, *emphasis*, and `[text](url)` links
//
// Not in the subset: raw HTML (it is escaped like any other text), tables,
// images, nested lists, and indented code blocks. A block that is not in
// the subset is a paragraph, so the failure mode is visible, not fatal.
//
// Blocks carry their one-based starting line so a build error can name a
// place in the source file; inline markup is rendered by `renderInline`
// with the caller's link resolver, not at parse time.

export interface HeadingBlock {
  type: "heading";
  level: number;
  text: string;
  line: number;
}

export interface ParagraphBlock {
  type: "paragraph";
  text: string;
  line: number;
}

export interface CodeBlock {
  type: "code";
  language: string;
  text: string;
  line: number;
}

export interface ListBlock {
  type: "list";
  ordered: boolean;
  items: string[];
  line: number;
}

export interface BlockquoteBlock {
  type: "blockquote";
  text: string;
  line: number;
}

export type Block =
  | HeadingBlock
  | ParagraphBlock
  | CodeBlock
  | ListBlock
  | BlockquoteBlock;

export type ResolveLink = (url: string) => string;

const FENCE = /^```(.*)$/;
const HEADING = /^(#{1,6})\s+(.*)$/;
const UNORDERED = /^[-*]\s+(.*)$/;
const ORDERED = /^\d+\.\s+(.*)$/;
const QUOTE = /^>\s?(.*)$/;

function isBlockStart(line: string): boolean {
  return (
    line.trim() === "" ||
    FENCE.test(line) ||
    HEADING.test(line) ||
    UNORDERED.test(line) ||
    ORDERED.test(line) ||
    QUOTE.test(line)
  );
}

export function parseMarkdown(source: string): Block[] {
  const lines = source.split("\n");
  const blocks: Block[] = [];
  let index = 0;

  while (index < lines.length) {
    const line = lines[index] ?? "";
    if (line.trim() === "") {
      index++;
      continue;
    }

    const fence = FENCE.exec(line);
    if (fence !== null) {
      const language = (fence[1] ?? "").trim();
      const start = index + 1;
      index++;
      const body: string[] = [];
      let closed = false;
      while (index < lines.length) {
        const current = lines[index] ?? "";
        if (current.startsWith("```")) {
          closed = true;
          index++;
          break;
        }
        body.push(current);
        index++;
      }
      if (!closed) {
        throw new Error(`${start}: the code fence is never closed`);
      }
      blocks.push({ type: "code", language, text: body.join("\n"), line: start });
      continue;
    }

    const heading = HEADING.exec(line);
    if (heading !== null) {
      blocks.push({
        type: "heading",
        level: (heading[1] ?? "#").length,
        text: (heading[2] ?? "").trim(),
        line: index + 1,
      });
      index++;
      continue;
    }

    const quote = QUOTE.exec(line);
    if (quote !== null) {
      const start = index + 1;
      const parts: string[] = [];
      while (index < lines.length) {
        const match = QUOTE.exec(lines[index] ?? "");
        if (match === null) {
          break;
        }
        parts.push((match[1] ?? "").trim());
        index++;
      }
      blocks.push({
        type: "blockquote",
        text: parts.join(" ").trim(),
        line: start,
      });
      continue;
    }

    const item = UNORDERED.exec(line) ?? ORDERED.exec(line);
    if (item !== null) {
      const ordered = UNORDERED.exec(line) === null;
      const start = index + 1;
      const items: string[] = [];
      while (index < lines.length) {
        const current = lines[index] ?? "";
        const match = ordered ? ORDERED.exec(current) : UNORDERED.exec(current);
        if (match !== null) {
          items.push((match[1] ?? "").trim());
          index++;
          continue;
        }
        if (current.startsWith("  ") && items.length > 0) {
          const last = items.length - 1;
          items[last] = `${items[last] ?? ""} ${current.trim()}`;
          index++;
          continue;
        }
        break;
      }
      blocks.push({ type: "list", ordered, items, line: start });
      continue;
    }

    const start = index + 1;
    const parts: string[] = [];
    while (index < lines.length && !isBlockStart(lines[index] ?? "")) {
      parts.push((lines[index] ?? "").trim());
      index++;
    }
    blocks.push({ type: "paragraph", text: parts.join(" "), line: start });
  }

  return blocks;
}

function escapeText(text: string): string {
  return text.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
}

function escapeAttribute(text: string): string {
  return escapeText(text).replaceAll('"', "&quot;");
}

// Renders the inline markup of one block. `resolveLink` rewrites and
// audits link targets; the default keeps the URL unchanged so a caller
// with nothing to check can pass nothing.
export function renderInline(text: string, resolveLink?: ResolveLink): string {
  return renderInlineWith(text, resolveLink ?? ((url: string) => url), true);
}

// The `]` matching the `[` at `start`, skipping nested brackets, or -1.
function labelEnd(text: string, start: number): number {
  let depth = 0;
  for (let index = start; index < text.length; index++) {
    const char = text[index];
    if (char === "\\") {
      index++;
    } else if (char === "[") {
      depth++;
    } else if (char === "]") {
      depth--;
      if (depth === 0) {
        return index;
      }
    }
  }
  return -1;
}

function renderInlineWith(
  text: string,
  resolve: ResolveLink,
  allowLinks: boolean,
): string {
  let html = "";
  let index = 0;

  while (index < text.length) {
    const char = text[index] ?? "";
    if (char === "\\" && index + 1 < text.length) {
      html += escapeText(text[index + 1] ?? "");
      index += 2;
      continue;
    }
    if (char === "`") {
      const end = text.indexOf("`", index + 1);
      if (end !== -1) {
        html += `<code>${escapeText(text.slice(index + 1, end))}</code>`;
        index = end + 1;
        continue;
      }
    }
    if (allowLinks && char === "[") {
      const close = labelEnd(text, index);
      if (close !== -1 && text[close + 1] === "(") {
        const urlEnd = text.indexOf(")", close + 2);
        if (urlEnd !== -1) {
          const label = text.slice(index + 1, close);
          const url = text.slice(close + 2, urlEnd);
          html += `<a href="${escapeAttribute(resolve(url))}">${renderInlineWith(label, resolve, false)}</a>`;
          index = urlEnd + 1;
          continue;
        }
      }
    }
    if (text.startsWith("**", index)) {
      const end = text.indexOf("**", index + 2);
      if (end !== -1) {
        html += `<strong>${renderInlineWith(text.slice(index + 2, end), resolve, false)}</strong>`;
        index = end + 2;
        continue;
      }
    }
    if (char === "*") {
      const end = text.indexOf("*", index + 1);
      if (end !== -1) {
        html += `<em>${renderInlineWith(text.slice(index + 1, end), resolve, false)}</em>`;
        index = end + 1;
        continue;
      }
    }
    html += escapeText(char);
    index++;
  }

  return html;
}
