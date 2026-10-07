// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Syntax highlighting through the tree-sitter grammar in `treesitter/`.
// The grammar wasm and queries are built into the site by
// `tools/site.py`; nothing here runs the compiler. The classes and the
// HTML runs are `../shared/highlight.js`; this module only owns the
// binding, the parser, and the incremental tree.

import { renderHighlightedHtml } from "../shared/highlight.js";
import { Parser, Language, Query } from "./vendor/web-tree-sitter.js";

export interface Highlighter {
  highlight(text: string): string;
  dispose(): void;
}

interface HighlighterOptions {
  // Bytes fetched by the caller let the runtime, grammar, and query load
  // together; the URL is the fallback when the binding fetches its own.
  runtimeWasm?: ArrayBuffer;
  runtimeWasmUrl?: string;
  languageWasm: Uint8Array | string;
  querySource: string;
}

export async function createHighlighter({
  runtimeWasm,
  runtimeWasmUrl,
  languageWasm,
  querySource,
}: HighlighterOptions): Promise<Highlighter> {
  await Parser.init(
    runtimeWasm
      ? { wasmBinary: runtimeWasm }
      : {
          locateFile: (name: string) =>
            name.endsWith(".wasm") ? (runtimeWasmUrl ?? name) : name,
        },
  );
  const language = await Language.load(languageWasm);
  const query = new Query(language, querySource);
  const parser = new Parser();
  parser.setLanguage(language);

  let previousTree: ReturnType<Parser["parse"]> | null = null;

  return {
    highlight(text: string): string {
      const tree = parser.parse(text);
      const captures = query.captures(tree.rootNode);
      previousTree?.delete();
      previousTree = tree;
      return renderHighlightedHtml(text, captures);
    },

    dispose(): void {
      previousTree?.delete();
      previousTree = null;
      query.delete();
      parser.delete();
    },
  };
}
