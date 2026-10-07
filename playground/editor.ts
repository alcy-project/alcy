// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The editor: a textarea the user types into, a highlighted mirror under
// it, and the line-number gutter beside it. The mirror and gutter follow
// the textarea's scrolling by hand, so this module owns the geometry that
// keeps the three in step.

import { elements } from "./elements.js";
import type { MessageKey, MessageParams } from "./i18n.js";
import { state } from "./state.js";

export const FALLBACK_SOURCE = `fn main() {\n  println("Hello, alcy!")\n}\n`;

export interface EditorHandlers {
  onInput?: () => void;
  onRun?: () => void;
  onHighlightError?: (key: MessageKey, params?: MessageParams) => void;
}

// Wired by `initEditor` so this module stays a leaf: errors it cannot fix
// are reported through the page's status line by the caller.
let onInput: () => void = () => {};
let onRun: () => void = () => {};
let onHighlightError: (key: MessageKey, params?: MessageParams) => void = () => {};

export function normalizedSource(): string {
  return elements.source.value.replaceAll("\r\n", "\n");
}

function updateGutter(): void {
  const lines = elements.source.value.split("\n").length;
  if (lines === state.gutterLines) {
    return;
  }
  state.gutterLines = lines;
  elements.gutter.textContent = Array.from({ length: lines }, (_, i) => i + 1).join("\n");
  elements.gutter.style.minWidth = `${String(lines).length + 1}ch`;
}

export function syncScroll(): void {
  // iOS overscroll reports offsets past either end of the range while
  // rubber-banding. The mirror clamps those silently when they are
  // assigned to its scrollTop, but the transform would move the gutter
  // alone; clamp here so both sides see the same value. The metrics are
  // read before anything is written, which keeps this a clean-layout
  // query on a scroll frame.
  const source = elements.source;
  const maxTop = Math.max(0, source.scrollHeight - source.clientHeight);
  const maxLeft = Math.max(0, source.scrollWidth - source.clientWidth);
  const top = Math.min(Math.max(source.scrollTop, 0), maxTop);
  const left = Math.min(Math.max(source.scrollLeft, 0), maxLeft);
  elements.highlight.scrollTop = top;
  elements.highlight.scrollLeft = left;
  elements.gutter.style.transform = `translateY(${-top}px)`;
}

// The mirror hides its scrollbars while the textarea shows them, so it
// has to pad by the space they take or the two clamp at different scroll
// maxima. The ResizeObserver on the textarea calls this when a scrollbar
// appears or disappears, which is the only time the numbers change.
export function syncMetrics(): void {
  const scrollbarWidth = elements.source.offsetWidth - elements.source.clientWidth;
  const scrollbarHeight = elements.source.offsetHeight - elements.source.clientHeight;
  elements.highlight.style.setProperty(
    "--editor-scrollbar-width",
    `${Math.max(0, scrollbarWidth)}px`,
  );
  elements.highlight.style.setProperty(
    "--editor-scrollbar-height",
    `${Math.max(0, scrollbarHeight)}px`,
  );
}

function insertAtCursor(text: string): void {
  const start = elements.source.selectionStart;
  const end = elements.source.selectionEnd;
  const value = elements.source.value;
  elements.source.value = value.slice(0, start) + text + value.slice(end);
  elements.source.selectionStart = elements.source.selectionEnd = start + text.length;
}

// The mirror is plain text the moment the textarea changes, so the glyphs
// under the caret are never stale; the colors arrive when the parse that
// produces them is done. A parse that cannot run (or fails) leaves the
// plain text in place.
function updateMirrorPlain(): void {
  state.highlightedText = null;
  elements.highlightCode.textContent = elements.source.value;
  syncScroll();
}

function scheduleHighlight(): void {
  updateMirrorPlain();
  if (state.highlightFrame !== null) {
    return;
  }
  state.highlightFrame = window.requestAnimationFrame(() => {
    state.highlightFrame = null;
    renderHighlight();
  });
}

function renderHighlight(): void {
  if (state.highlighter === null) {
    return;
  }
  const text = elements.source.value;
  if (text === state.highlightedText) {
    return;
  }
  try {
    elements.highlightCode.innerHTML = state.highlighter.highlight(text);
    state.highlightedText = text;
    syncScroll();
  } catch (error) {
    state.highlighter = null;
    state.highlightedText = null;
    onHighlightError("status.highlightingDisabled", {
      detail: error instanceof Error ? error.message : String(error),
    });
    updateMirrorPlain();
  }
}

// Colors are an enhancement, so the runtime, the grammar, and the query
// are fetched together once the page is idle: first paint and typing do
// not wait for a wasm download. Plain text is already on screen.
function whenIdle(callback: () => void): void {
  if (typeof window.requestIdleCallback === "function") {
    window.requestIdleCallback(callback, { timeout: 1500 });
  } else {
    window.setTimeout(callback, 200);
  }
}

async function initHighlighter(): Promise<void> {
  const runtimeUrl = new URL("vendor/web-tree-sitter.wasm", document.baseURI).href;
  const grammarUrl = new URL("grammar/tree-sitter-alcy.wasm", document.baseURI).href;
  try {
    const [module, queryResponse, runtimeBytes, grammarBytes] = await Promise.all([
      import("./highlight.js"),
      fetch("grammar/highlights.scm"),
      fetch(runtimeUrl).then((response) => response.arrayBuffer()),
      fetch(grammarUrl).then((response) => response.arrayBuffer()),
    ]);
    state.highlighter = await module.createHighlighter({
      runtimeWasm: runtimeBytes,
      languageWasm: new Uint8Array(grammarBytes),
      querySource: await queryResponse.text(),
    });
    renderHighlight();
  } catch (error) {
    onHighlightError("status.highlightingUnavailable", {
      detail: error instanceof Error ? error.message : String(error),
    });
  }
}

function noteEdit(): void {
  scheduleHighlight();
  updateGutter();
  onInput();
}

// Replaces the buffer from the outside (a sample, or the restored
// session) and puts the editor back at the top.
export function setText(text: string): void {
  elements.source.value = text;
  scheduleHighlight();
  updateGutter();
  syncMetrics();
  elements.source.scrollTop = 0;
  syncScroll();
}

export function initEditor(handlers: EditorHandlers): void {
  onInput = handlers.onInput ?? onInput;
  onRun = handlers.onRun ?? onRun;
  onHighlightError = handlers.onHighlightError ?? onHighlightError;

  elements.source.value = FALLBACK_SOURCE;
  updateGutter();
  syncMetrics();
  updateMirrorPlain();

  elements.source.addEventListener("input", noteEdit);
  elements.source.addEventListener("scroll", syncScroll);
  // The wheel over the line numbers belongs to the editor, not to the
  // gutter, which is not a scroll container.
  elements.gutter.addEventListener(
    "wheel",
    (event) => {
      elements.source.scrollTop += event.deltaY;
      elements.source.scrollLeft += event.deltaX;
      event.preventDefault();
    },
    { passive: false },
  );
  elements.source.addEventListener("keydown", (event) => {
    if (event.key === "Tab") {
      event.preventDefault();
      insertAtCursor("  ");
      noteEdit();
    } else if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
      event.preventDefault();
      onRun();
    }
  });

  new ResizeObserver(syncMetrics).observe(elements.source);
  whenIdle(() => void initHighlighter());
}
