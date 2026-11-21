// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Editing a guide page's `alcy` block: Edit and Done, the mirror under
// the caret, and Reset. A page loads it only when a block holds a
// complete program, and it stays separate from `guide-run.js` so a
// reader who only presses Run never waits for the editor or the colors.
// Open is wired in `guide.js`, which loads with the page.

import type { Highlighter } from "../playground/highlight.js";
import { findBlocks, type CodeBlock } from "./guide-copy.js";
import { t } from "./site.js";

// The colors come from the playground's copy of the grammar and its
// runtime; the module's own URL decides where those are, in any tree.
const PLAYGROUND_BASE = new URL("../playground/", import.meta.url);

// The runtime highlighter, shared by every editable block and loaded on
// the first Edit: colors while typing are an enhancement, so a reader
// who never edits never downloads the grammar, and a load that fails
// leaves the plain mirror -- the same text, uncolored -- doing the
// same job.
let highlighterPromise: Promise<Highlighter | null> | null = null;
let highlighter: Highlighter | null = null;
const mirrorRefreshes: Array<() => void> = [];
// Every press after the module is loaded arrives here again; wire a block
// once so its Open button does not collect a second listener.
const wiredBlocks = new WeakSet<CodeBlock>();
// One open editor per page: opening another block closes this one, so a
// second editing frame cannot stay behind.
let activeEditor: (() => void) | null = null;

function ensureHighlighter(): Promise<Highlighter | null> {
  if (highlighterPromise === null) {
    highlighterPromise = (async () => {
      try {
        const [module, query, runtime, grammar] = await Promise.all([
          import("../playground/highlight.js"),
          fetch(new URL("grammar/highlights.scm", PLAYGROUND_BASE)),
          fetch(new URL("vendor/web-tree-sitter.wasm", PLAYGROUND_BASE)).then(
            (response) => response.arrayBuffer(),
          ),
          fetch(new URL("grammar/tree-sitter-alcy.wasm", PLAYGROUND_BASE)).then(
            (response) => response.arrayBuffer(),
          ),
        ]);
        const ready = await module.createHighlighter({
          runtimeWasm: runtime,
          languageWasm: new Uint8Array(grammar),
          querySource: await query.text(),
        });
        highlighter = ready;
        // Blocks already showing a plain mirror get their colors now.
        for (const refresh of mirrorRefreshes) {
          refresh();
        }
        return ready;
      } catch {
        return null;
      }
    })();
  }
  return highlighterPromise;
}

function createEditor(source: string): HTMLTextAreaElement {
  const editor = document.createElement("textarea");
  editor.className = "code-editor";
  editor.value = source;
  editor.spellcheck = false;
  editor.setAttribute("autocomplete", "off");
  editor.setAttribute("autocapitalize", "off");
  editor.setAttribute("autocorrect", "off");
  editor.setAttribute("wrap", "off");
  editor.setAttribute("aria-label", t("guide.edit"));
  return editor;
}

// Installs the whole editor on a block and, when `pressed` is set, treats
// the press that caused this module to load as the Edit it asked for.
export function wireEditor(block: CodeBlock, pressed = false): void {
  if (wiredBlocks.has(block)) {
    return;
  }
  wiredBlocks.add(block);

  const edit = block.actions.querySelector<HTMLButtonElement>('[data-action="edit"]');
  const reset = block.actions.querySelector<HTMLButtonElement>('[data-action="reset"]');

  // A block may ship read-only, like the 404's: no working copy and no
  // editing state, only the published text and its buttons.
  if (edit === null || reset === null) {
    return;
  }
  const published = block.code.textContent ?? "";
  // The highlighted markup, put back whenever the working copy is the
  // published text again.
  const publishedHtml = block.code.innerHTML;
  let working = published;
  let editor: HTMLTextAreaElement | null = null;

  const modified = (): boolean => working !== published;

  const syncReset = (): void => {
    reset.hidden = !modified();
  };

  // The pre beneath the editing overlay is the mirror: the same bytes
  // the reader is typing, colored once the highlighter is up and plain
  // until then.
  const paint = (text: string): void => {
    if (highlighter === null) {
      block.code.textContent = text;
    } else {
      block.code.innerHTML = highlighter.highlight(text);
    }
  };

  // Done shows the working copy, not the published one: an edit survives
  // leaving the textarea, and only Reset takes it back. The colors come
  // from the runtime highlighter when it is up; until then the copy
  // reads as plain text.
  const showWorking = (): void => {
    if (modified()) {
      paint(working);
    } else {
      block.code.innerHTML = publishedHtml;
    }
  };

  // Repaint whenever the highlighter arrives late: editing blocks show
  // the caret's own text, a finished edit its working copy.
  const refreshMirror = (): void => {
    if (editor !== null) {
      paint(editor.value);
    } else {
      showWorking();
    }
  };
  mirrorRefreshes.push(refreshMirror);

  // Copy and Run follow the reader, not the published text.
  block.source = () => (editor === null ? working : editor.value);

  const closeEditor = (): void => {
    if (editor === null) {
      return;
    }
    working = editor.value;
    editor.remove();
    editor = null;
    block.root.classList.remove("editing");
    showWorking();
    edit.textContent = t("guide.edit");
    edit.setAttribute("aria-pressed", "false");
    syncReset();
    if (activeEditor === closeEditor) {
      activeEditor = null;
    }
  };

  const toggleEditor = (): void => {
    if (editor === null) {
      activeEditor?.();
      const created = createEditor(working);
      editor = created;
      created.addEventListener("input", () => {
        working = created.value;
        paint(working);
        syncReset();
      });
      block.pre.after(created);
      // The frame reads as active the moment Edit is pressed, and the
      // caret is already in the text, so typing needs no second click.
      block.root.classList.add("editing");
      created.focus();
      edit.textContent = t("guide.done");
      edit.setAttribute("aria-pressed", "true");
      activeEditor = closeEditor;
      void ensureHighlighter();
    } else {
      closeEditor();
    }
  };

  edit.addEventListener("click", toggleEditor);
  if (pressed) {
    toggleEditor();
  }

  reset.addEventListener("click", () => {
    working = published;
    if (editor !== null) {
      editor.value = published;
      block.code.innerHTML = publishedHtml;
    } else {
      showWorking();
    }
    syncReset();
  });
}
