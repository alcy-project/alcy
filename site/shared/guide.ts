// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's code blocks. The generator stamps each block's buttons and
// leaves them hidden, so a reader without JavaScript sees only the code;
// this module reveals them and wires Copy on every block, and Run and
// Edit on the `alcy` ones. The `alcy` blocks are complete programs by
// construction -- the build compiles them -- so Run is the playground's
// compiler and runner over the block's own text, loaded on first use.

import { openCompiler, runProgram } from "./compiler-client.js";
import type {
  CompilerClient,
  CompilerResponse,
  CompilerStatusName,
  Diagnostic,
  RunOutcome,
} from "./compiler-client.js";
import { t } from "./site.js";

// The workers live in the playground's one set of assets; the module's
// own URL decides where that is, in any language tree.
const PLAYGROUND_BASE = new URL("../playground/", import.meta.url);
const COPIED_MS = 1500;
const RUN_TIMEOUT_MS = 5000;

interface CodeBlock {
  pre: HTMLElement;
  code: HTMLElement;
  actions: HTMLElement;
  output: HTMLElement | null;
  alcy: boolean;
}

function findBlocks(): CodeBlock[] {
  const found: CodeBlock[] = [];
  for (const root of document.querySelectorAll<HTMLElement>(".code-block")) {
    const pre = root.querySelector<HTMLElement>("pre");
    const code = root.querySelector<HTMLElement>("pre code");
    const actions = root.querySelector<HTMLElement>(".code-actions");
    if (pre !== null && code !== null && actions !== null) {
      found.push({
        pre,
        code,
        actions,
        output: root.querySelector<HTMLElement>(".code-output"),
        alcy: root.dataset.language === "alcy",
      });
    }
  }
  return found;
}

async function writeClipboard(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    // A denied clipboard or an insecure context: the old path still
    // copies, and a failure just leaves the button as it was.
    const scratch = document.createElement("textarea");
    scratch.value = text;
    scratch.setAttribute("readonly", "");
    scratch.style.position = "fixed";
    scratch.style.opacity = "0";
    document.body.append(scratch);
    scratch.select();
    let copied = false;
    try {
      copied = document.execCommand("copy");
    } catch {
      copied = false;
    }
    scratch.remove();
    return copied;
  }
}

function copy(block: CodeBlock, button: HTMLButtonElement): void {
  void writeClipboard(block.code.textContent ?? "").then((copied) => {
    if (!copied) {
      return;
    }
    button.textContent = t("guide.copied");
    window.setTimeout(() => {
      button.textContent = t("guide.copy");
    }, COPIED_MS);
  });
}

function wireCopy(block: CodeBlock): void {
  const button = block.actions.querySelector<HTMLButtonElement>('[data-action="copy"]');
  if (button === null) {
    return;
  }
  button.addEventListener("click", () => copy(block, button));
}

// One run at a time per page: the compiler worker is shared, and a second
// click would only queue behind the first.
let compiler: CompilerClient | null = null;
let currentState: HTMLElement | null = null;
let running = false;

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

function createEditor(block: CodeBlock, source: string): HTMLTextAreaElement {
  const editor = document.createElement("textarea");
  editor.className = "code-editor";
  editor.value = source;
  editor.rows = Math.max(2, source.split("\n").length);
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
  if (edit === null || reset === null || run === null) {
    return;
  }

  const original = block.code.textContent ?? "";
  let editor: HTMLTextAreaElement | null = null;

  const syncReset = (): void => {
    reset.hidden = editor === null || editor.value === original;
  };

  edit.addEventListener("click", () => {
    if (editor === null) {
      editor = createEditor(block, original);
      editor.addEventListener("input", syncReset);
      block.pre.hidden = true;
      block.pre.after(editor);
      edit.textContent = t("guide.done");
      edit.setAttribute("aria-pressed", "true");
    } else {
      editor.remove();
      editor = null;
      block.pre.hidden = false;
      edit.textContent = t("guide.edit");
      edit.setAttribute("aria-pressed", "false");
    }
    syncReset();
  });

  reset.addEventListener("click", () => {
    if (editor !== null) {
      editor.value = original;
      editor.rows = Math.max(2, original.split("\n").length);
      syncReset();
    }
  });

  run.addEventListener("click", () => {
    void compileAndRun(block, editor === null ? original : editor.value, run);
  });
}

for (const block of findBlocks()) {
  block.actions.hidden = false;
  wireCopy(block);
  if (block.alcy) {
    wireAlcy(block);
  }
}
