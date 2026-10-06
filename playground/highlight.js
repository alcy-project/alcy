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

const ROOT_PRIORITY = {
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

function priority(name) {
  return ROOT_PRIORITY[name.split(".")[0]] ?? 2;
}

function classesFor(name) {
  const parts = name.split(".");
  const classes = [];
  let prefix = "";
  for (const part of parts) {
    prefix = prefix ? `${prefix}-${part}` : part;
    classes.push(`hl-${prefix}`);
  }
  return classes.join(" ");
}

function escapeHtml(text) {
  return text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

// `runtimeWasm` and `languageWasm` may be bytes, which lets the page
// fetch the runtime, the grammar, and the query at the same time;
// `runtimeWasmUrl` is the fallback for a caller that would rather let
// the binding fetch the runtime itself. `querySource` is the text of
// `queries/alcy/highlights.scm`.
export async function createHighlighter({
  runtimeWasm,
  runtimeWasmUrl,
  languageWasm,
  querySource,
}) {
  await Parser.init(
    runtimeWasm
      ? { wasmBinary: runtimeWasm }
      : {
          locateFile: (name) =>
            name.endsWith(".wasm") ? runtimeWasmUrl : name,
        },
  );
  const language = await Language.load(languageWasm);
  const query = new Query(language, querySource);
  const parser = new Parser();
  parser.setLanguage(language);

  let previousTree = null;
  const captureIds = new Map();
  const captureNames = [];
  const captureClasses = [];

  return {
    // Returns HTML for the whole text, one `<span class="hl-...">` run
    // per highlight. Node indices are UTF-16 code units, matching the
    // string a `<textarea>` holds, so no byte mapping is needed here.
    highlight(text) {
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
        const id = classes[index];
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

    dispose() {
      previousTree?.delete();
      previousTree = null;
      query.delete();
      parser.delete();
    },
  };
}
