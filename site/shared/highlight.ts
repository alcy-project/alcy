// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The rendering half of syntax highlighting, shared by the playground's
// editor (which parses as you type) and the site generator (which
// highlights the guide's code at build time). Turning grammar captures
// into CSS classes and classes into runs of HTML is the same in both;
// only the tree-sitter binding and the way it is loaded differ.
//
// The capture names in `highlights.scm` follow the usual tree-sitter
// convention (`keyword.control`, `function.method`, ...). A capture maps
// to one CSS class per ancestor, so a theme can style `keyword` once and
// override `keyword.control` where it wants to.
//
// Captures overwrite each other by priority rather than by query order:
// the grammar ends with a plain `variable` capture that is meant to lose
// to the `function`, `type`, and `property` captures above it.

export interface Capture {
  name: string;
  node: { startIndex: number; endIndex: number };
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

export function escapeHtml(text: string): string {
  return text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

// Returns HTML for the whole text, one `<span class="hl-...">` run per
// highlight. Node indices are UTF-16 code units, matching the string the
// caller parsed, so no byte mapping is needed here.
export function renderHighlightedHtml(
  text: string,
  captures: readonly Capture[],
): string {
  const classes = new Int32Array(text.length);
  const captureIds = new Map<string, number>();
  const captureClasses: string[] = [];
  const sorted = [...captures].sort((a, b) => priority(a.name) - priority(b.name));
  for (const capture of sorted) {
    let id = captureIds.get(capture.name);
    if (id === undefined) {
      captureClasses.push(classesFor(capture.name));
      id = captureClasses.length;
      captureIds.set(capture.name, id);
    }
    const start = Math.max(0, Math.min(text.length, capture.node.startIndex));
    const end = Math.max(start, Math.min(text.length, capture.node.endIndex));
    if (start !== end) {
      classes.fill(id, start, end);
    }
  }

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
}
