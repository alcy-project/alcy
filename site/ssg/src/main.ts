// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's generator. It reads `docs/guide/`, checks every `alcy`
// fence against the playground's own compiler wasm module, highlights
// the code with the tree-sitter grammar, audits every link, and writes
// the pages under `<dist>/guide/`.
//
//     node site/build/ssg/src/main.js \
//       --docs docs/guide --dist site/dist --repo-root . \
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

import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { pathToFileURL } from "node:url";

import { escapeHtml } from "../../shared/highlight.js";
import { discoverGuide } from "./guide.js";
import type { GuidePage } from "./guide.js";
import { createHighlighter } from "./highlight.js";
import type { Highlighter } from "./highlight.js";
import { createLinkChecker } from "./links.js";
import { renderGuideIndex, renderGuidePage } from "./render.js";

interface Options {
  docs: string;
  dist: string;
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
      "               [--glue <js>] [--grammar <wasm> --highlights <scm> --tree-sitter <js>]\n",
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
  pages: readonly GuidePage[],
  compiler: AlcyCompiler | null,
): { count: number; errors: string[] } {
  const errors: string[] = [];
  let count = 0;
  if (compiler === null) {
    return { count, errors };
  }
  for (const page of pages) {
    for (const block of page.blocks) {
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
      errors.push(`${page.sourceRel}:${block.line}: ${message}`);
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

async function main(): Promise<void> {
  const options = parseArgs(process.argv.slice(2));
  const pages = discoverGuide(options.docs);
  if (pages.length === 0) {
    throw new Error(`${options.docs} has no guide pages`);
  }

  const compiler = await loadCompiler(options);
  const fences = validateFences(pages, compiler);
  const highlighter = await loadHighlighter(options);
  const highlight = (code: string): string =>
    highlighter === null ? escapeHtml(code) : highlighter.highlight(code);

  const checker = createLinkChecker({
    pages,
    siteExists: (route) => siteExists(options.dist, route),
    repoExists: (path) => existsSync(join(options.repoRoot, path)),
  });

  for (const page of pages) {
    const html = renderGuidePage(page, {
      pages,
      highlight,
      resolveLink: (url) => checker.resolve(url, page),
    });
    const target = join(options.dist, page.route);
    mkdirSync(dirname(target), { recursive: true });
    writeFileSync(target, html);
  }
  writeFileSync(join(options.dist, "guide", "index.html"), renderGuideIndex(pages[0] as GuidePage));

  const errors = [...fences.errors, ...checker.errors];
  if (errors.length > 0) {
    for (const error of errors) {
      process.stderr.write(`guide: ${error}\n`);
    }
    process.exitCode = 1;
    return;
  }
  process.stdout.write(`guide: ${pages.length} pages, ${fences.count} alcy fences compiled\n`);
}

await main();
