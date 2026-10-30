// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Editing a guide page's `alcy` block: Edit and Done, the mirror under
// the caret, Reset, and the hand-off to the playground. A page loads it
// only when a block holds a complete program, and it stays separate from
// `guide-run.js` so a reader who only presses Run never waits for the
// editor, the colors, or the hand-off.

import { handOffToPlayground } from "./handoff.js";
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
  const edit = block.actions.querySelector<HTMLButtonElement>('[data-action="edit"]');
  const reset = block.actions.querySelector<HTMLButtonElement>('[data-action="reset"]');
  const open = block.actions.querySelector<HTMLButtonElement>('[data-action="open"]');

  // The 404's block ships without one: its own links already reach the
  // playground, and its depth under the deployment root is whatever
  // was mistyped, so there is no "../" that lands there.
  if (open !== null) {
    open.addEventListener("click", () => {
      // The handoff rides sessionStorage, and the reader keeps their own
      // tree's playground, one directory up from this page.
      handOffToPlayground(block.source());
      window.location.assign(new URL("../playground/", window.location.href).href);
    });
  }

  // A block may ship read-only, like the 404's: no working copy and no
  // editing state, only the published text and its buttons.
  if (edit === null || reset === null) {
    return;
  }
  // The page hands over a block whose Edit was pressed while this module
  // was still arriving; a second press in that window would open the
  // editor twice, so the flag is spent once and the button is the owner.
  if (edit.dataset.wired === "true") {
    return;
  }
  edit.dataset.wired = "true";

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

  const toggleEditor = (): void => {
    if (editor === null) {
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
      void ensureHighlighter();
    } else {
      working = editor.value;
      editor.remove();
      editor = null;
      block.root.classList.remove("editing");
      showWorking();
      edit.textContent = t("guide.edit");
      edit.setAttribute("aria-pressed", "false");
    }
    syncReset();
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
