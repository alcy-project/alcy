// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The build-time highlighter: the same rendering as the playground's
// editor (`shared/highlight.ts`), loaded against the node copy of the
// tree-sitter binding instead of the vendored browser bundle. The
// binding, the grammar, and the query are named by the build tool.

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

import { renderHighlightedHtml } from "../../shared/highlight.js";
import type { Capture } from "../../shared/highlight.js";

interface TsNode {
  startIndex: number;
  endIndex: number;
}

interface TsTree {
  rootNode: TsNode;
  delete(): void;
}

interface TsQuery {
  captures(node: TsNode): Capture[];
}

interface TsParser {
  setLanguage(language: TsLanguage): void;
  parse(text: string): TsTree;
}

interface TsLanguage {}

interface TsBinding {
  Parser: {
    init(options?: { locateFile?(name: string): string }): Promise<void>;
    new (): TsParser;
  };
  Language: {
    load(input: Uint8Array): Promise<TsLanguage>;
  };
  Query: new (language: TsLanguage, source: string) => TsQuery;
}

export interface Highlighter {
  highlight(code: string): string;
}

export interface HighlighterOptions {
  bindingPath: string;
  runtimeWasmPath: string;
  grammarWasmPath: string;
  queryPath: string;
}

export async function createHighlighter(
  options: HighlighterOptions,
): Promise<Highlighter> {
  const binding = (await import(
    pathToFileURL(options.bindingPath).href
  )) as unknown as TsBinding;
  await binding.Parser.init({
    locateFile: (name: string) =>
      name.endsWith(".wasm") ? options.runtimeWasmPath : name,
  });
  const language = await binding.Language.load(readFileSync(options.grammarWasmPath));
  const query = new binding.Query(language, readFileSync(options.queryPath, "utf8"));
  const parser = new binding.Parser();
  parser.setLanguage(language);

  return {
    highlight(code: string): string {
      const tree = parser.parse(code);
      const captures = query.captures(tree.rootNode);
      const html = renderHighlightedHtml(code, captures);
      tree.delete();
      return html;
    },
  };
}
