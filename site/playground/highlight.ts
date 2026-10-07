// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Syntax highlighting through the tree-sitter grammar in `treesitter/`.
// The grammar wasm and queries are built into the site by
// `tools/playground.py`; nothing here runs the compiler.
//
// The capture names in `highlights.scm` follow the usual tree-sitter
// convention (`keyword.control`, `function.method`, ...). A capture maps
// to one CSS class per ancestor, so a theme can style `keyword` once and
// override `keyword.control` where it wants to.
//
// Captures overwrite each other by priority rather than by query order:
// the grammar ends with a plain `variable` capture that is meant to lose
// to the `function`, `type`, and `property` captures above it.

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

const ROOT_PRIORITY: Record<string, number> = {
  comment: 3,
  constant: 3,
  keyword: 3,
  label: 3,
  number: 3,
  string: 3,
  operator: 2,
  punctuation: 2,
  variable: 1,
  constructor: 4,
  function: 4,
  namespace: 4,
  property: 4,
  type: 4,
};

function priority(name: string): number {
  return ROOT_PRIORITY[name.split(".")[0] ?? ""] ?? 2;
}

function classesFor(name: string): string {
  const parts = name.split(".");
  const classes: string[] = [];
  let prefix = "";
  for (const part of parts) {
    prefix = prefix ? `${prefix}-${part}` : part;
    classes.push(`hl-${prefix}`);
  }
  return classes.join(" ");
}

function escapeHtml(text: string): string {
  return text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
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
  const captureIds = new Map<string, number>();
  const captureNames: string[] = [];
  const captureClasses: string[] = [];

  return {
    // Returns HTML for the whole text, one `<span class="hl-...">` run
    // per highlight. Node indices are UTF-16 code units, matching the
    // string a `<textarea>` holds, so no byte mapping is needed here.
    highlight(text: string): string {
      const tree = parser.parse(text);
      const classes = new Int32Array(text.length);
      const captures = query.captures(tree.rootNode);
      captures.sort((a, b) => priority(a.name) - priority(b.name));
      for (const capture of captures) {
        let id = captureIds.get(capture.name);
        if (id === undefined) {
          captureNames.push(capture.name);
          captureClasses.push(classesFor(capture.name));
          id = captureNames.length;
          captureIds.set(capture.name, id);
        }
        const start = Math.max(0, Math.min(text.length, capture.node.startIndex));
        const end = Math.max(start, Math.min(text.length, capture.node.endIndex));
        if (start !== end) {
          classes.fill(id, start, end);
        }
      }
      previousTree?.delete();
      previousTree = tree;

      let html = "";
      let index = 0;
      while (index < text.length) {
        const id = classes[index] ?? 0;
        let end = index + 1;
        while (end < text.length && classes[end] === id) {
          end++;
        }
        const chunk = escapeHtml(text.slice(index, end));
        if (id === 0) {
          html += chunk;
        } else {
          html += `<span class="${captureClasses[id - 1]}">${chunk}</span>`;
        }
        index = end;
      }
      return html;
    },

    dispose(): void {
      previousTree?.delete();
      previousTree = null;
      query.delete();
      parser.delete();
    },
  };
}
