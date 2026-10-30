// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's pages: the shell every page shares, the article its blocks
// render to, the contents beside it, the previous/next pager, and the
// contents page at the tree's `/guide/`. Every label is stamped in the
// page's language, so nothing has to be translated at runtime.

import { posix } from "node:path";

import { escapeHtml } from "../../shared/highlight.js";
import { translate } from "../../shared/i18n.js";
import type { MessageKey } from "../../shared/i18n.js";
import type { GuidePage } from "./guide.js";
import { otherLanguages, prefixOf } from "./languages.js";
import type { Language } from "./languages.js";
import { renderInline } from "./markdown.js";
import type { Block } from "./markdown.js";
import { renderSettings } from "./shell.js";
import { pageLinks, SITE_PATH, socialMeta } from "./urls.js";

export interface PageOptions {
  pages: readonly GuidePage[];
  highlight(code: string): string;
  resolveLink(url: string): string;
  // The theme bootstrap, inlined into the head: a render-blocking
  // request before first paint is a round trip on every cold load, and
  // the script changes only when the pages do.
  boot: string;
  // The build's provenance, shown in the footer.
  stamp: Stamp;
}

// Where this build came from: the tag of a release build, the commit's
// short sha otherwise.
export interface Stamp {
  text: string;
  url: string;
}

// The prefix from a page's directory to the built site's root, where the
// shared assets live.
function assetPrefix(route: string): string {
  const depth = route.split("/").length - 1;
  return depth === 0 ? "" : "../".repeat(depth);
}

// The prefix from a page's directory to its language tree's root.
function treeHome(route: string, language: Language): string {
  const root = prefixOf(language).replace(/\/$/, "");
  const relative = posix.relative(posix.dirname(route), root);
  return relative === "" ? "./" : `${relative}/`;
}

// The URL of the same tree path in the page's other language.
function languageSwitch(route: string, language: Language, treePath: string): string {
  const other = otherLanguages(language)[0];
  if (other === undefined) {
    return "";
  }
  return posix.relative(posix.dirname(route), prefixOf(other) + treePath);
}

function relativeRoute(from: string, to: string): string {
  return posix.relative(posix.dirname(from), to);
}

function header(route: string, language: Language, treePath: string): string {
  const home = treeHome(route, language);
  const t = (key: MessageKey): string => translate(language, key);
  const settings = renderSettings({
    language,
    switchHref: languageSwitch(route, language, treePath),
  });
  return `<header class="site-header">
      <a class="site-brand" href="${home}">alcy</a>
      <nav class="site-nav">
        <a href="${home}guide/" aria-current="page">${t("site.guide")}</a>
        <a href="${home}playground/">${t("site.playground")}</a>
        <button type="button" class="site-search-open" data-action="search">${t("site.search")}</button>
      </nav>
      ${settings}
    </header>`;
}

// The footer's provenance: the build's stamp, linked to the commit or
// tag it names when there is somewhere to point.
export function stampMark(stamp: Stamp): string {
  const text = escapeHtml(stamp.text);
  return stamp.url === ""
    ? text
    : `<a href="${stamp.url}" rel="noopener">${text}</a>`;
}

function footer(stamp: Stamp): string {
  return `<footer class="site-footer">
      <span>alcy · ${stampMark(stamp)}</span>
      <a href="https://github.com/alcy-project/alcy" rel="noopener">GitHub</a>
    </footer>`;
}

// The slug a heading anchors at: lowercased, with every run a URL
// dislikes collapsed to a single dash.
function headingSlug(text: string): string {
  return text
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");
}

export interface GuideSection {
  id: string;
  text: string;
  block: Block;
}

// The page's sections: its level-2 headings, each with an id unique on
// the page. The reader gets the outline beside the article, a link can
// cite a section, and the search index knows where the text lives.
export function sectionsOf(page: GuidePage): GuideSection[] {
  const used = new Set<string>();
  const sections: GuideSection[] = [];
  for (const block of page.blocks) {
    if (block.type !== "heading" || block.level !== 2) {
      continue;
    }
    const base = headingSlug(block.text);
    let id = base;
    for (let count = 2; used.has(id); count += 1) {
      id = `${base}-${count}`;
    }
    used.add(id);
    sections.push({ id, text: block.text, block });
  }
  return sections;
}

function blockHtml(
  block: Block,
  options: PageOptions,
  t: (key: MessageKey) => string,
  headingIds: ReadonlyMap<Block, string>,
): string {
  switch (block.type) {
    case "heading": {
      const id = headingIds.get(block);
      const anchor = id === undefined ? "" : ` id="${id}"`;
      return `<h${block.level}${anchor}>${renderInline(block.text, options.resolveLink)}</h${block.level}>`;
    }
    case "paragraph":
      return `<p>${renderInline(block.text, options.resolveLink)}</p>`;
    case "code": {
      const isAlcy = block.language === "alcy";
      const code = isAlcy ? options.highlight(block.text) : escapeHtml(block.text);
      const actions = [
        `<button type="button" data-action="copy">${t("guide.copy")}</button>`,
        ...(isAlcy
          ? [
              `<button type="button" data-action="edit" aria-pressed="false">${t("guide.edit")}</button>`,
              `<button type="button" data-action="reset" hidden>${t("guide.reset")}</button>`,
              `<button type="button" data-action="run">${t("button.run")}</button>`,
              `<button type="button" data-action="open" title="${t("guide.openTitle")}">${t("guide.open")}</button>`,
            ]
          : []),
      ].join("\n          ");
      const output = isAlcy ? `\n        <div class="code-output" hidden></div>` : "";
      const language = isAlcy ? ` data-language="alcy"` : "";
      // `code-view` is the positioning context for the editing overlay:
      // the pre is the highlight mirror the transparent textarea sits on.
      return `<div class="code-block"${language}>
        <div class="code-view">
          <pre><code>${code}</code></pre>
        </div>
        <div class="code-actions" hidden>
          ${actions}
        </div>${output}
      </div>`;
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
  const t = (key: MessageKey): string => translate(page.language, key);
  const items = options.pages
    .map((other) => {
      const current = other === page ? ' aria-current="page"' : "";
      return `<li><a href="${relativeRoute(page.route, other.route)}"${current}>${escapeHtml(other.title)}</a></li>`;
    })
    .join("\n        ");
  return `<nav class="guide-nav" aria-label="${t("site.guide")}">
      <h2>${t("site.guide")}</h2>
      <ol>
        ${items}
      </ol>
    </nav>`;
}

// "On this page": the article's own sections. Two or more earn an
// outline; fewer than that would only repeat the title.
function outline(
  sections: readonly GuideSection[],
  options: PageOptions,
  t: (key: MessageKey) => string,
): string {
  if (sections.length < 2) {
    return "";
  }
  const items = sections
    .map(
      (section) =>
        `<li><a href="#${section.id}">${renderInline(section.text, options.resolveLink)}</a></li>`,
    )
    .join("\n        ");
  return `<details class="guide-outline">
      <summary>${t("guide.onThisPage")}</summary>
      <ol>
        ${items}
      </ol>
    </details>`;
}

function pager(page: GuidePage, options: PageOptions): string {
  const t = (key: MessageKey): string => translate(page.language, key);
  const index = options.pages.indexOf(page);
  const previous = index > 0 ? options.pages[index - 1] : undefined;
  const next =
    index >= 0 && index + 1 < options.pages.length ? options.pages[index + 1] : undefined;
  const back = previous
    ? `<a href="${relativeRoute(page.route, previous.route)}">← ${t("site.previous")} · ${escapeHtml(previous.title)}</a>`
    : `<span class="empty"></span>`;
  const forward = next
    ? `<a href="${relativeRoute(page.route, next.route)}">${t("site.next")} · ${escapeHtml(next.title)} →</a>`
    : `<span class="empty"></span>`;
  return `<nav class="guide-pager" aria-label="${t("site.guide")}">
        ${back}
        ${forward}
      </nav>`;
}

const SPECULATION = "<script type=\"speculationrules\">" +
  '{"prerender":[{"source":"document","where":{"href_matches":"/*"},"eagerness":"moderate"}],' +
  '"prefetch":[{"source":"document","where":{"href_matches":"/*"},"eagerness":"moderate"}]}' +
  "</script>";

export function renderGuidePage(page: GuidePage, options: PageOptions): string {
  const assets = assetPrefix(page.route);
  const t = (key: MessageKey): string => translate(page.language, key);
  const hasCode = page.blocks.some((block) => block.type === "code");
  const guideCss = hasCode
    ? `\n    <link rel="stylesheet" href="${assets}shared/guide.css" />`
    : "";
  const guideJs = hasCode
    ? `\n    <script type="module" src="${assets}shared/guide.js"></script>`
    : "";
  const sections = sectionsOf(page);
  const headingIds = new Map<Block, string>(sections.map((s) => [s.block, s.id]));
  const article = page.blocks
    .map((block) => blockHtml(block, options, t, headingIds))
    .join("\n      ");
  const description = `${page.title} · ${t("meta.guide")}`;
  return `<!doctype html>
<!-- Copyright 2026 The Alcy Project Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->
<html lang="${page.language}">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="description" content="${escapeHtml(description)}" />
    <title>${escapeHtml(page.title)} · alcy</title>
    <script>${options.boot}</script>
    <link rel="stylesheet" href="${assets}shared/site.css" />${guideCss}
    <link rel="modulepreload" href="${assets}shared/shell.js" />
    ${hasCode ? `<link rel="modulepreload" href="${assets}shared/guide.js" />` : ""}
    <link rel="icon" href="${assets}favicon.svg" />
    ${pageLinks(page.language, page.path)}
    ${socialMeta(`${page.title} · alcy`, description, page.route)}
    ${SPECULATION}
  </head>
  <body class="site">
    ${header(page.route, page.language, page.path)}

    <div class="guide-layout">
      <div class="guide-aside">
        ${contents(page, options)}
        ${outline(sections, options, t)}
      </div>

      <main class="guide-article">
        ${article}
        ${pager(page, options)}
      </main>
    </div>

    ${footer(options.stamp)}

    <script type="module" src="${assets}shared/shell.js"></script>${guideJs}
  </body>
</html>
`;
}

// `/guide/` is a real page, not a signpost: a contents list a reader can
// land on, so following the header's Guide link never flashes empty.
export function renderGuideIndex(
  pages: readonly GuidePage[],
  language: Language,
  boot: string,
  stamp: Stamp,
): string {
  const route = `${prefixOf(language)}guide/index.html`;
  const assets = assetPrefix(route);
  const t = (key: MessageKey): string => translate(language, key);
  const items = pages
    .map(
      (page) =>
        `<li><a href="${relativeRoute(route, page.route)}">${escapeHtml(page.title)}</a></li>`,
    )
    .join("\n        ");
  return `<!doctype html>
<!-- Copyright 2026 The Alcy Project Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->
<html lang="${language}">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="description" content="${escapeHtml(t("meta.guide"))}" />
    <title>${t("site.guide")} · alcy</title>
    <script>${boot}</script>
    <link rel="stylesheet" href="${assets}shared/site.css" />
    <link rel="modulepreload" href="${assets}shared/shell.js" />
    <link rel="icon" href="${assets}favicon.svg" />
    ${pageLinks(language, "guide/index.html")}
    ${socialMeta(`${t("site.guide")} · alcy`, t("meta.guide"), route)}
    ${SPECULATION}
  </head>
  <body class="site">
    ${header(route, language, "guide/index.html")}

    <main class="site-main guide-contents">
      <h1>${t("site.guide")}</h1>
      <ol>
        ${items}
      </ol>
    </main>

    ${footer(stamp)}

    <script type="module" src="${assets}shared/shell.js"></script>
  </body>
</html>
`;
}

// The page GitHub Pages serves for a path that does not exist. It is one
// file at the site root, and Pages answers the requested path with its
// content, so every URL here is written from the deployment root and the
// script below picks that root: the project's path on Pages, or the
// server's root when the built directory is served locally.
// The 404 carries one runnable block: the page is missing, the compiler
// is not, and a program that exits 404 says so better than prose. The
// block is the guide's, read-only -- no Edit, no Reset, no Open; the
// page's own links already reach the playground, and its depth under
// the deployment root is whatever was mistyped.
export function renderNotFound(
  boot: string,
  stamp: Stamp,
  highlight: (code: string) => string,
): string {
  const source = `fn main() -> i32 {
  println("Page not found")
  ret 404
}`;
  return `<!doctype html>
<!-- Copyright 2026 The Alcy Project Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="robots" content="noindex" />
    <title>Page not found · alcy</title>
    <script>
      (() => {
        const base = document.createElement("base");
        base.href = location.pathname.startsWith("${SITE_PATH}") ? "${SITE_PATH}" : "/";
        document.head.append(base);
      })();
    </script>
    <script>${boot}</script>
    <link rel="stylesheet" href="shared/site.css" />
    <link rel="stylesheet" href="shared/guide.css" />
    <link rel="icon" href="favicon.svg" />
  </head>
  <body class="site">
    <header class="site-header">
      <a class="site-brand" href="./">alcy</a>
      <nav class="site-nav">
        <a href="guide/">Guide</a>
        <a href="playground/">Playground</a>
      </nav>
    </header>

    <main class="site-main">
      <h1>Page not found</h1>
      <p>
        The page you asked for does not exist. It may have moved, or the link
        that brought you here may be wrong.
      </p>
      <div class="actions">
        <a class="primary" href="./">Go home</a>
        <a href="guide/">Read the guide</a>
        <a href="playground/">Open the playground</a>
      </div>
      <div class="code-block" data-language="alcy">
        <div class="code-view">
          <pre><code>${highlight(source)}</code></pre>
        </div>
        <div class="code-actions" hidden>
          <button type="button" data-action="copy">Copy</button>
          <button type="button" data-action="run">Run</button>
        </div>
        <div class="code-output" hidden></div>
      </div>
    </main>

    ${footer(stamp)}
    <script type="module" src="shared/guide.js"></script>
  </body>
</html>
`;
}
