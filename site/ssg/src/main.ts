// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The site's generator. It reads `docs/guide/`, checks every `alcy`
// fence against the playground's own compiler wasm module, highlights
// the code with the tree-sitter grammar, audits every link, and writes
// one page tree per language: English at the site root and every other
// language under its own prefix. The landing page and the playground
// page are templates under `site/`; the generator stamps them into each
// tree too, so a page never renders in the wrong language first.
//
//     bun site/build/ssg/src/main.js \
//       --docs docs/guide --dist site/dist --site site --repo-root . \
//       --alcy src/playground/js/alcy.mjs \
//       --glue site/dist/playground/src/alcy_playground.js \
//       --grammar site/dist/playground/grammar/tree-sitter-alcy.wasm \
//       --highlights site/dist/playground/grammar/highlights.scm \
//       --tree-sitter out/site-cache/deps/node_modules/web-tree-sitter/web-tree-sitter.js
//
// The compiler and the grammar are optional so a build without the wasm
// artifacts still produces pages (with escaped, unchecked code); the
// Pages workflow builds the compiler first, so a deployed guide always
// carries compiled examples.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, posix } from "node:path";
import { pathToFileURL } from "node:url";

import { escapeHtml } from "../../shared/highlight.js";
import { LANGUAGES, translate } from "../../shared/i18n.js";
import { discoverGuide, pagesForLanguage } from "./guide.js";
import type { GuidePage, GuideSource } from "./guide.js";
import { createHighlighter } from "./highlight.js";
import type { Highlighter } from "./highlight.js";
import { otherLanguages, prefixOf } from "./languages.js";
import type { Language } from "./languages.js";
import { createLinkChecker } from "./links.js";
import { renderGuideIndex, renderGuidePage, renderNotFound, sectionsOf } from "./render.js";
import type { Stamp } from "./render.js";
import { stampMark } from "./render.js";
import { renderSettings } from "./shell.js";
import { stampTemplate } from "./templates.js";
import type { TemplateValues } from "./templates.js";
import { canonicalUrl, pageLinks, SITE_URL, socialMeta } from "./urls.js";

interface Options {
  docs: string;
  dist: string;
  site: string;
  repoRoot: string;
  alcy: string;
  glue?: string;
  grammar?: string;
  highlights?: string;
  treeSitter?: string;
  stamp: string;
  stampUrl: string;
}

interface Diagnostic {
  severity: string;
  message: string;
}

interface AlcyResult {
  ok: boolean;
  diagnostics: Diagnostic[];
}

interface AlcyCompiler {
  compile(source: string): AlcyResult;
}

interface AlcyWrapper {
  createPlayground(options: { factory: unknown }): Promise<AlcyCompiler>;
}

function usage(): never {
  process.stderr.write(
    "usage: main.js --docs <dir> --dist <dir> --repo-root <dir> --alcy <wrapper>\n" +
      "               [--site <dir>] [--glue <js>]\n" +
      "               [--grammar <wasm> --highlights <scm> --tree-sitter <js>]\n",
  );
  process.exit(64);
}

function parseArgs(argv: string[]): Options {
  const values = new Map<string, string>();
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (key === undefined || value === undefined || !key.startsWith("--")) {
      usage();
    }
    values.set(key, value);
  }
  const docs = values.get("--docs");
  const dist = values.get("--dist");
  const repoRoot = values.get("--repo-root");
  const alcy = values.get("--alcy");
  if (docs === undefined || dist === undefined || repoRoot === undefined || alcy === undefined) {
    usage();
  }
  const options: Options = {
    docs,
    dist,
    site: values.get("--site") ?? join(repoRoot, "site"),
    repoRoot,
    alcy,
    glue: values.get("--glue"),
    grammar: values.get("--grammar"),
    highlights: values.get("--highlights"),
    treeSitter: values.get("--tree-sitter"),
    stamp: values.get("--stamp") ?? "unknown",
    stampUrl: values.get("--stamp-url") ?? "",
  };
  const grammarParts = [options.grammar, options.highlights, options.treeSitter];
  if (grammarParts.some((part) => part !== undefined) && grammarParts.some((part) => part === undefined)) {
    usage();
  }
  return options;
}

async function loadCompiler(options: Options): Promise<AlcyCompiler | null> {
  if (options.glue === undefined) {
    process.stdout.write("guide: no compiler wasm; alcy fences were not compiled\n");
    return null;
  }
  const glue = (await import(pathToFileURL(options.glue).href)) as { default: unknown };
  const wrapper = (await import(
    pathToFileURL(options.alcy).href
  )) as unknown as AlcyWrapper;
  return wrapper.createPlayground({ factory: glue.default });
}

// Every `alcy` fence is a complete program, so the compiler that the
// playground runs is the one that accepts it. The first error of a fence
// is reported; the diagnostics after it are usually consequences.
function validateFences(
  sources: readonly GuideSource[],
  compiler: AlcyCompiler | null,
): { count: number; errors: string[] } {
  const errors: string[] = [];
  let count = 0;
  if (compiler === null) {
    return { count, errors };
  }
  for (const source of sources) {
    for (const block of source.blocks) {
      if (block.type !== "code" || block.language !== "alcy") {
        continue;
      }
      count++;
      const result = compiler.compile(block.text);
      if (result.ok) {
        continue;
      }
      const first = result.diagnostics.find((entry) => entry.severity === "error");
      const message = first === undefined ? "the compiler refused the fence" : first.message;
      errors.push(`${source.sourceRel}:${block.line}: ${message}`);
    }
  }
  return { count, errors };
}

async function loadHighlighter(options: Options): Promise<Highlighter | null> {
  if (options.grammar === undefined || options.highlights === undefined || options.treeSitter === undefined) {
    process.stdout.write("guide: no grammar wasm; code fences are not highlighted\n");
    return null;
  }
  return createHighlighter({
    bindingPath: options.treeSitter,
    runtimeWasmPath: join(dirname(options.treeSitter), "web-tree-sitter.wasm"),
    grammarWasmPath: options.grammar,
    queryPath: options.highlights,
  });
}

function siteExists(dist: string, route: string): boolean {
  if (route === "") {
    return existsSync(join(dist, "index.html"));
  }
  if (route.endsWith("/")) {
    return existsSync(join(dist, route, "index.html"));
  }
  return existsSync(join(dist, route));
}

// The social card a template's page shows: titled for what the page is,
// pointing at its own canonical address.
function templateSocial(language: Language, treePath: string, route: string): string {
  if (treePath === "playground/index.html") {
    const title = `alcy ${translate(language, "site.playground")}`;
    return socialMeta(title, translate(language, "meta.playground"), route);
  }
  return socialMeta(
    "alcy",
    "An experimental statically-typed programming language.",
    route,
  );
}

// The values a page template needs for one language: where the shared
// assets are, where the language tree's root is, the link to the same
// page in the other language, the settings menu, the absolute links a
// crawler reads, and the social card.
function templateValues(
  language: Language,
  treePath: string,
  boot: string,
  stamp: Stamp,
): TemplateValues {
  const route = prefixOf(language) + treePath;
  const dir = posix.dirname(route);
  const dist = posix.relative(dir, ".");
  const root = prefixOf(language).replace(/\/$/, "");
  const home = posix.relative(dir, root);
  const other = otherLanguages(language)[0];
  const switchHref =
    other === undefined ? "" : posix.relative(dir, prefixOf(other) + treePath);
  return {
    lang: language,
    dist: dist === "" ? "" : `${dist}/`,
    home: home === "" ? "./" : `${home}/`,
    switch: switchHref,
    settings: renderSettings({ language, switchHref }),
    pageLinks: pageLinks(language, treePath),
    social: templateSocial(language, treePath, route),
    boot: `<script>${boot}</script>`,
    stamp: stampMark(stamp),
  };
}

function writePage(dist: string, route: string, html: string): void {
  const target = join(dist, route);
  mkdirSync(dirname(target), { recursive: true });
  writeFileSync(target, html);
}

// The files that belong to the site rather than to one page tree: the
// 404 page, the icon, and the two files crawlers read.
function writeSiteRootFiles(
  options: Options,
  pages: readonly GuidePage[],
  boot: string,
  stamp: Stamp,
  highlight: (code: string) => string,
): void {
  writeFileSync(join(options.dist, "404.html"), renderNotFound(boot, stamp, highlight));
  writeFileSync(
    join(options.dist, "favicon.svg"),
    readFileSync(join(options.site, "favicon.svg")),
  );
  writeFileSync(
    join(options.dist, "og.png"),
    readFileSync(join(options.site, "og.png")),
  );
  writeFileSync(
    join(options.dist, "robots.txt"),
    `User-agent: *\nAllow: /\n\nSitemap: ${SITE_URL}sitemap.xml\n`,
  );
  const routes = LANGUAGES.flatMap((language) => [
    `${prefixOf(language)}index.html`,
    ...pages
      .filter((page) => page.language === language)
      .map((page) => page.route),
    `${prefixOf(language)}guide/index.html`,
    `${prefixOf(language)}playground/index.html`,
  ]);
  const entries = routes
    .map((route) => `  <url>\n    <loc>${canonicalUrl(route)}</loc>\n  </url>`)
    .join("\n");
  writeFileSync(
    join(options.dist, "sitemap.xml"),
    `<?xml version="1.0" encoding="UTF-8"?>\n` +
      `<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n` +
      `${entries}\n</urlset>\n`,
  );
  // The search index covers the trees' shared content once; the module
  // that reads it resolves a route into the reader's own tree.
  const guide = pages
    .filter((page) => page.language === "en")
    .map((page) => ({
      route: page.route,
      title: page.title,
      sections: sectionsOf(page).map((section) => ({
        id: section.id,
        text: section.text,
      })),
    }));
  writeFileSync(
    join(options.dist, "search-index.json"),
    JSON.stringify(guide, null, 2) + "\n",
  );
}

// The theme bootstrap travels inline in every page's head rather than
// as its own request: a render-blocking script is a round trip before
// first paint on every cold load. It must contain nothing a script tag
// ends on.
function readBoot(site: string): string {
  const boot = readFileSync(join(site, "shared", "boot.js"), "utf8");
  if (boot.includes("</script")) {
    throw new Error("shared/boot.js cannot be inlined: it contains </script");
  }
  return boot;
}

async function main(): Promise<void> {
  const options = parseArgs(process.argv.slice(2));
  const stamp: Stamp = { text: options.stamp, url: options.stampUrl };
  const sources = discoverGuide(options.docs);
  if (sources.length === 0) {
    throw new Error(`${options.docs} has no guide pages`);
  }

  const compiler = await loadCompiler(options);
  const fences = validateFences(sources, compiler);
  const highlighter = await loadHighlighter(options);
  const highlighted = new Map<string, string>();
  const highlight = (code: string): string => {
    const ready = highlighted.get(code);
    if (ready !== undefined) {
      return ready;
    }
    const html = highlighter === null ? escapeHtml(code) : highlighter.highlight(code);
    highlighted.set(code, html);
    return html;
  };

  const allPages = LANGUAGES.flatMap((language) => pagesForLanguage(sources, language));
  const checker = createLinkChecker({
    pages: allPages,
    siteExists: (route) => siteExists(options.dist, route),
    repoExists: (path) => existsSync(join(options.repoRoot, path)),
  });

  const landingTemplate = readFileSync(join(options.site, "index.html"), "utf8");
  const playgroundTemplate = readFileSync(
    join(options.site, "playground", "index.html"),
    "utf8",
  );
  const boot = readBoot(options.site);
  const templates: Array<{ treePath: string; source: string }> = [
    { treePath: "index.html", source: landingTemplate },
    { treePath: "playground/index.html", source: playgroundTemplate },
  ];

  // The templates and the contents pages first: they create the routes
  // the guide's own links point at, so the link audit in the second pass
  // sees the whole site.
  for (const language of LANGUAGES) {
    const pages: readonly GuidePage[] = allPages.filter(
      (page) => page.language === language,
    );
    writePage(
      options.dist,
      `${prefixOf(language)}guide/index.html`,
      renderGuideIndex(pages, language, boot, stamp),
    );
    for (const template of templates) {
      writePage(
        options.dist,
        prefixOf(language) + template.treePath,
        stampTemplate(
          template.source,
          language,
          templateValues(language, template.treePath, boot, stamp),
        ),
      );
    }
  }

  for (const language of LANGUAGES) {
    const pages: readonly GuidePage[] = allPages.filter(
      (page) => page.language === language,
    );
    for (const page of pages) {
      writePage(
        options.dist,
        page.route,
        renderGuidePage(page, {
          pages,
          highlight,
          resolveLink: (url) => checker.resolve(url, page),
          boot,
          stamp,
        }),
      );
    }
  }

  writeSiteRootFiles(options, allPages, boot, stamp, highlight);

  const errors = [...fences.errors, ...checker.errors];
  if (errors.length > 0) {
    for (const error of errors) {
      process.stderr.write(`guide: ${error}\n`);
    }
    process.exitCode = 1;
    return;
  }
  process.stdout.write(
    `guide: ${sources.length} pages × ${LANGUAGES.length} languages, ` +
      `${fences.count} alcy fences compiled\n`,
  );
}

await main();
