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
//     node site/build/ssg/src/main.js \
//       --docs docs/guide --dist site/dist --site site --repo-root . \
//       --alcy compiler/playground/js/alcy.mjs \
//       --glue site/dist/playground/compiler/alcy_playground.js \
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
import { LANGUAGES } from "../../shared/i18n.js";
import { discoverGuide, pagesForLanguage } from "./guide.js";
import type { GuidePage, GuideSource } from "./guide.js";
import { createHighlighter } from "./highlight.js";
import type { Highlighter } from "./highlight.js";
import { otherLanguages, prefixOf } from "./languages.js";
import type { Language } from "./languages.js";
import { createLinkChecker } from "./links.js";
import { renderGuideIndex, renderGuidePage } from "./render.js";
import { renderSettings } from "./shell.js";
import { stampTemplate } from "./templates.js";
import type { TemplateValues } from "./templates.js";

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

// The values a page template needs for one language: where the shared
// assets are, where the language tree's root is, the link to the same
// page in the other language, and the settings menu itself.
function templateValues(language: Language, treePath: string): TemplateValues {
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
  };
}

function writePage(dist: string, route: string, html: string): void {
  const target = join(dist, route);
  mkdirSync(dirname(target), { recursive: true });
  writeFileSync(target, html);
}

async function main(): Promise<void> {
  const options = parseArgs(process.argv.slice(2));
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
      renderGuideIndex(pages, language),
    );
    for (const template of templates) {
      writePage(
        options.dist,
        prefixOf(language) + template.treePath,
        stampTemplate(template.source, language, templateValues(language, template.treePath)),
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
        }),
      );
    }
  }

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
