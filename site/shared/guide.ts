// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The `alcy` code blocks' Run and Edit. A guide page loads this module
// only when a block holds a complete program, so a page of ordinary
// examples does not carry the compiler's client or the editor. Copy is
// every page's, and lives in `guide-copy.js`.

import { handOffToPlayground } from "./handoff.js";
import { openCompiler, runProgram } from "./compiler-client.js";
import type {
  CompilerClient,
  CompilerResponse,
  CompilerStatusName,
  Diagnostic,
  RunOutcome,
} from "./compiler-client.js";
import type { Highlighter } from "../playground/highlight.js";
import { findBlocks, type CodeBlock } from "./guide-copy.js";
import { t } from "./site.js";

// The workers live in the playground's one set of assets; the module's
// own URL decides where that is, in any language tree.
const PLAYGROUND_BASE = new URL("../playground/", import.meta.url);
const RUN_TIMEOUT_MS = 5000;

// One run at a time per page: the compiler worker is shared, and a second
// click would only queue behind the first.
let compiler: CompilerClient | null = null;
let currentState: HTMLElement | null = null;
let running = false;

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

function runButtons(): HTMLButtonElement[] {
  return [...document.querySelectorAll<HTMLButtonElement>('.code-block [data-action="run"]')];
}

function disableOtherRuns(active: HTMLButtonElement | null): void {
  for (const button of runButtons()) {
    button.disabled = active !== null && button !== active;
  }
}

function onCompilerStatus(status: CompilerStatusName, detail: string): void {
  if (currentState === null) {
    return;
  }
  if (status === "loading") {
    currentState.textContent = t("compiler.loading");
  } else if (status === "ready") {
    currentState.textContent = t("status.compiling");
  } else if (status === "error") {
    currentState.textContent = detail === "" ? t("compiler.error") : detail;
  }
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

function appendState(output: HTMLElement, message: string): void {
  const state = document.createElement("p");
  state.className = "code-state";
  state.textContent = message;
  output.append(state);
}

function appendStream(output: HTMLElement, label: string, text: string): void {
  if (text === "") {
    return;
  }
  const name = document.createElement("p");
  name.className = "code-label";
  name.textContent = label;
  const stream = document.createElement("pre");
  stream.className = "code-stream";
  stream.textContent = text;
  output.append(name, stream);
}

function renderFailure(output: HTMLElement, label: string, detail: string): void {
  const meta = document.createElement("p");
  meta.className = "code-meta";
  meta.textContent = label;
  output.replaceChildren(meta);
  appendState(output, detail);
}

function renderDiagnostics(output: HTMLElement, diagnostics: readonly Diagnostic[]): void {
  const errors = diagnostics.filter((diagnostic) => diagnostic.severity === "error");
  const shown = errors.length > 0 ? errors : diagnostics;
  const meta = document.createElement("p");
  meta.className = "code-meta";
  meta.textContent = t("status.compileFailed");
  const list = document.createElement("ul");
  list.className = "code-problems";
  for (const diagnostic of shown) {
    const item = document.createElement("li");
    item.className = `severity-${diagnostic.severity}`;
    item.textContent = diagnostic.message;
    list.append(item);
  }
  output.replaceChildren(meta, list);
}

function renderOutcome(
  output: HTMLElement,
  response: CompilerResponse,
  bytes: number,
  outcome: RunOutcome,
): void {
  const parts: string[] = [];
  const compileMs = response.ms?.toFixed(1);
  if (compileMs !== undefined) {
    parts.push(
      t("meta.compiled", {
        ms: compileMs,
        bytes,
        functions: response.stats?.functionCount ?? 0,
      }),
    );
  }
  switch (outcome.type) {
    case "result":
      parts.push(t("meta.exitCode", { code: outcome.exitCode }));
      parts.push(t("meta.ran", { ms: outcome.ms.toFixed(1) }));
      break;
    case "timeout":
      parts.push(t("meta.timedOut"));
      break;
    case "error":
      parts.push(t("meta.notRunnable"));
      break;
  }
  const meta = document.createElement("p");
  meta.className = "code-meta";
  meta.textContent = parts.join(" · ");
  output.replaceChildren(meta);
  if (outcome.type === "result") {
    appendStream(output, "stdout", outcome.stdout);
    appendStream(output, "stderr", outcome.stderr);
  } else if (outcome.type === "timeout") {
    appendState(output, t("program.timedOut", { seconds: RUN_TIMEOUT_MS / 1000 }));
  } else {
    appendState(output, outcome.message);
  }
}

async function compileAndRun(
  block: CodeBlock,
  source: string,
  button: HTMLButtonElement,
): Promise<void> {
  const output = block.output;
  if (running || output === null) {
    return;
  }
  running = true;
  button.textContent = t("button.running");
  disableOtherRuns(button);

  const state = document.createElement("p");
  state.className = "code-state";
  state.textContent = t("status.compiling");
  output.hidden = false;
  output.replaceChildren(state);
  currentState = state;
  output.scrollIntoView({ block: "nearest" });

  try {
    if (compiler === null) {
      compiler = openCompiler({ base: PLAYGROUND_BASE, onStatus: onCompilerStatus });
    }
    const response = await compiler.request("compile", source);
    if (response.error !== undefined) {
      renderFailure(output, t("status.compilerUnavailable"), response.error);
      return;
    }
    if (!response.ok || response.wasm === null) {
      renderDiagnostics(output, response.diagnostics);
      return;
    }
    state.textContent = t("status.running");
    // The runner takes ownership of the bytes; remember their size before
    // the transfer detaches the buffer.
    const bytes = response.wasm.byteLength;
    const outcome = await runProgram({
      base: PLAYGROUND_BASE,
      wasm: response.wasm,
      timeoutMs: RUN_TIMEOUT_MS,
    });
    renderOutcome(output, response, bytes, outcome);
  } catch (error) {
    renderFailure(
      output,
      t("status.compilerError"),
      error instanceof Error ? error.message : String(error),
    );
  } finally {
    running = false;
    currentState = null;
    button.textContent = t("button.run");
    disableOtherRuns(null);
  }
}

function wireAlcy(block: CodeBlock): void {
  if (block.output === null) {
    return;
  }
  const edit = block.actions.querySelector<HTMLButtonElement>('[data-action="edit"]');
  const reset = block.actions.querySelector<HTMLButtonElement>('[data-action="reset"]');
  const run = block.actions.querySelector<HTMLButtonElement>('[data-action="run"]');
  const open = block.actions.querySelector<HTMLButtonElement>('[data-action="open"]');

  if (run !== null) {
    run.addEventListener("click", () => {
      void compileAndRun(block, block.source(), run);
    });
  }

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

  edit.addEventListener("click", () => {
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
  });

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

for (const block of findBlocks()) {
  if (block.alcy) {
    wireAlcy(block);
  }
}

