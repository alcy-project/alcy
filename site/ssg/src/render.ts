// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's pages: the shell every page shares, the article its blocks
// render to, the contents beside it, and the previous/next pager. The
// shell's controls carry the same ids as the landing page's and the
// playground's, so `shared/shell.js` wires them all the same way.

import { posix } from "node:path";

import { escapeHtml } from "../../shared/highlight.js";
import type { GuidePage } from "./guide.js";
import { renderInline } from "./markdown.js";
import type { Block } from "./markdown.js";

export interface PageOptions {
  pages: readonly GuidePage[];
  highlight(code: string): string;
  resolveLink(url: string): string;
}

// How to climb from a page's directory to the built site's root.
function prefix(route: string): string {
  const depth = route.split("/").length - 1;
  return "../".repeat(depth);
}

function relativeRoute(from: string, to: string): string {
  return posix.relative(posix.dirname(from), to);
}

function header(page: GuidePage): string {
  const up = prefix(page.route);
  return `<header class="site-header">
      <a class="site-brand" href="${up}">alcy</a>
      <nav class="site-nav">
        <a href="${up}guide/" data-i18n="site.guide">Guide</a>
        <a href="${up}playground/" data-i18n="site.playground">Playground</a>
        <a href="https://github.com/alcy-project/alcy" rel="noopener">GitHub</a>
      </nav>
      <div class="site-controls">
        <label class="control">
          <span class="control-label" data-i18n="label.theme">Theme</span>
          <select id="theme" aria-label="Color theme" data-i18n-aria="label.theme">
            <option value="auto" data-i18n="theme.auto">Auto</option>
            <option value="light" data-i18n="theme.light">Light</option>
            <option value="dark" data-i18n="theme.dark">Dark</option>
          </select>
        </label>
        <label class="control">
          <span class="control-label" data-i18n="label.language">Language</span>
          <select id="language" aria-label="Language" data-i18n-aria="label.language">
            <option value="auto" data-i18n="language.auto">Auto</option>
            <option value="en">English</option>
            <option value="ja">日本語</option>
          </select>
        </label>
      </div>
    </header>`;
}

function blockHtml(block: Block, page: GuidePage, options: PageOptions): string {
  switch (block.type) {
    case "heading": {
      const level = block.level;
      return `<h${level}>${renderInline(block.text, options.resolveLink)}</h${level}>`;
    }
    case "paragraph":
      return `<p>${renderInline(block.text, options.resolveLink)}</p>`;
    case "code": {
      const code = block.language === "alcy" ? options.highlight(block.text) : escapeHtml(block.text);
      return `<pre><code>${code}</code></pre>`;
    }
    case "list": {
      const tag = block.ordered ? "ol" : "ul";
      const items = block.items
        .map((item) => `<li>${renderInline(item, options.resolveLink)}</li>`)
        .join("\n        ");
      return `<${tag}>
        ${items}
      </${tag}>`;
    }
    case "blockquote":
      return `<blockquote><p>${renderInline(block.text, options.resolveLink)}</p></blockquote>`;
  }
}

function contents(page: GuidePage, options: PageOptions): string {
  const items = options.pages
    .map((other) => {
      const current = other === page ? ' aria-current="page"' : "";
      const href = relativeRoute(page.route, other.route);
      return `<li><a href="${href}"${current}>${escapeHtml(other.title)}</a></li>`;
    })
    .join("\n        ");
  return `<nav class="guide-nav" aria-label="Guide">
      <h2 data-i18n="site.guide">Guide</h2>
      <ol>
        ${items}
      </ol>
    </nav>`;
}

function pager(page: GuidePage, options: PageOptions): string {
  const index = options.pages.indexOf(page);
  const previous = index > 0 ? options.pages[index - 1] : undefined;
  const next = index >= 0 && index + 1 < options.pages.length ? options.pages[index + 1] : undefined;
  const back = previous
    ? `<a href="${relativeRoute(page.route, previous.route)}">← <span data-i18n="site.previous">Previous</span> · ${escapeHtml(previous.title)}</a>`
    : `<span class="empty"></span>`;
  const forward = next
    ? `<a href="${relativeRoute(page.route, next.route)}"><span data-i18n="site.next">Next</span> · ${escapeHtml(next.title)} →</a>`
    : `<span class="empty"></span>`;
  return `<nav class="guide-pager" aria-label="Guide pages">
        ${back}
        ${forward}
      </nav>`;
}

export function renderGuidePage(page: GuidePage, options: PageOptions): string {
  const up = prefix(page.route);
  const article = page.blocks
    .map((block) => blockHtml(block, page, options))
    .join("\n      ");
  return `<!doctype html>
<!-- Copyright 2026 The Alcy Project Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="description" content="${escapeHtml(page.title)} · the alcy guide" />
    <title>${escapeHtml(page.title)} · alcy</title>
    <script src="${up}shared/boot.js"></script>
    <link rel="stylesheet" href="${up}shared/site.css" />
  </head>
  <body class="site">
    ${header(page)}

    <div class="guide-layout">
      ${contents(page, options)}

      <main class="guide-article">
        ${article}
        ${pager(page, options)}
      </main>
    </div>

    <footer class="site-footer">
      <span>alcy · pre-MVP</span>
      <a href="https://github.com/alcy-project/alcy" rel="noopener">GitHub</a>
    </footer>

    <script type="module" src="${up}shared/shell.js"></script>
  </body>
</html>
`;
}

// `/guide/` is a signpost, not a page: it sends a reader to the first
// page without taking part in the contents.
export function renderGuideIndex(first: GuidePage): string {
  const href = posix.basename(first.route);
  return `<!doctype html>
<!-- Copyright 2026 The Alcy Project Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta http-equiv="refresh" content="0; url=${href}" />
    <title>alcy guide</title>
  </head>
  <body>
    <p><a href="${href}">alcy guide</a></p>
  </body>
</html>
`;
}
