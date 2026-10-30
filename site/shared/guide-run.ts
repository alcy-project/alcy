// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The `alcy` code blocks' Run. A guide page loads this module only when
// a block holds a complete program, so a page of ordinary examples does
// not carry the compiler's client. Copy is every page's, and lives in
// `guide-copy.js`; editing a program and handing it to the playground
// live in `guide-edit.js`.

import { openCompiler, runProgram } from "./compiler-client.js";
import type {
  CompilerClient,
  CompilerResponse,
  CompilerStatusName,
  Diagnostic,
  RunOutcome,
} from "./compiler-client.js";
import type { CodeBlock } from "./guide-copy.js";
import { t } from "./site.js";

// The workers live in the playground's one set of assets; the module's
// own URL decides where that is, in any language tree. The editor's
// half resolves the same directory from its own URL.
const PLAYGROUND_BASE = new URL("../playground/", import.meta.url);
const RUN_TIMEOUT_MS = 5000;

// One run at a time per page: the compiler worker is shared, and a second
// click would only queue behind the first.
let compiler: CompilerClient | null = null;
let currentState: HTMLElement | null = null;
let running = false;

function runButtons(): HTMLButtonElement[] {
  return [...document.querySelectorAll<HTMLButtonElement>('.code-block [data-action="run"]')];
}

export function disableOtherRuns(active: HTMLButtonElement | null): void {
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

export function renderFailure(output: HTMLElement, label: string, detail: string): void {
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

export function wireRun(block: CodeBlock): void {
  if (block.output === null) {
    return;
  }
  const run = block.actions.querySelector<HTMLButtonElement>('[data-action="run"]');
  if (run === null) {
    return;
  }
  run.addEventListener("click", () => {
    void compileAndRun(block, block.source(), run);
  });
}

