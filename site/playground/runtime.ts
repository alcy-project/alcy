// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The run side of the page: the compiler worker behind Check and the
// disposable runner worker behind Run.

import { elements } from "./elements.js";
import { normalizedSource } from "./editor.js";
import { t } from "./language.js";
import { setRunMeta, setRunMetaSuffix } from "./output.js";
import { renderDiagnostics } from "./problems.js";
import { state } from "./state.js";
import {
  setCompilerState,
  setRunning,
  setStatus,
  showCompilerError,
} from "./status.js";
import { showTab } from "./tabs.js";
import type {
  CompilerMessage,
  CompilerResponse,
  CompilerStatusName,
  RunnerMessage,
} from "./types.js";

const CHECK_DEBOUNCE_MS = 500;
const RUN_TIMEOUT_MS = 5000;

function ensureCompilerWorker(): Worker {
  if (state.compiler !== null) {
    return state.compiler;
  }
  const worker = new Worker("compiler.worker.js");
  worker.addEventListener("message", (event: MessageEvent<CompilerMessage>) => {
    const message = event.data;
    if (message.type === "status") {
      applyCompilerState(message.status, message.detail);
      return;
    }
    const pending = state.requests.get(message.id);
    if (pending === undefined) {
      return;
    }
    state.requests.delete(message.id);
    pending.resolve(message);
  });
  worker.addEventListener("error", (event) => {
    failAllRequests(event.message || "the compiler worker stopped");
    worker.terminate();
    state.compiler = null;
    applyCompilerState("error", event.message || "");
  });
  worker.addEventListener("messageerror", () => {
    failAllRequests("the compiler worker sent an unreadable message");
  });
  state.compiler = worker;
  return worker;
}

function failAllRequests(message: string): void {
  for (const pending of state.requests.values()) {
    pending.reject(new Error(message));
  }
  state.requests.clear();
}

function callCompiler(
  op: "check" | "compile",
  source: string,
): Promise<CompilerResponse> {
  const worker = ensureCompilerWorker();
  const id = ++state.requestSeq;
  return new Promise((resolve, reject) => {
    state.requests.set(id, { resolve, reject });
    worker.postMessage({ type: "request", id, op, source });
  });
}

// What a compiler state change means for the page: a ready worker gets a
// check scheduled, and a failed load shows the error diagnostic.
function applyCompilerState(status: CompilerStatusName, detail: string): void {
  setCompilerState(status, detail);
  if (status === "ready") {
    scheduleCheck();
  }
  if (status === "error") {
    showCompilerError(detail);
  }
}

export function scheduleCheck(): void {
  if (state.compilerStatus !== "ready") {
    return;
  }
  window.clearTimeout(state.checkTimer);
  state.checkTimer = window.setTimeout(() => {
    void runCheck({ automatic: true });
  }, CHECK_DEBOUNCE_MS);
}

export async function runCheck({
  automatic = false,
}: { automatic?: boolean } = {}): Promise<void> {
  if (state.running) {
    return;
  }
  const sequence = ++state.checkSeq;
  if (!automatic) {
    setStatus("status.checking");
  }
  try {
    const started = performance.now();
    const response = await callCompiler("check", normalizedSource());
    if (sequence !== state.checkSeq) {
      return;
    }
    if (response.error !== undefined) {
      // The load failure was already reported through the compiler state;
      // this response's empty diagnostics must not clear that report.
      return;
    }
    renderDiagnostics(response.diagnostics);
    if (!automatic) {
      showTab("problems");
      const ms = (performance.now() - started).toFixed(1);
      if (response.ok) {
        setStatus("status.checkOk", {
          functions: response.stats?.functionCount ?? 0,
          ms,
        });
      } else {
        setStatus("status.checkFailed", {
          count: response.diagnostics.length,
        });
      }
    }
  } catch (error) {
    if (sequence === state.checkSeq) {
      applyCompilerState("error", error instanceof Error ? error.message : String(error));
    }
  }
}

function execute(wasmBuffer: ArrayBuffer): Promise<void> {
  return new Promise((resolve) => {
    state.runner?.terminate();
    const runner = new Worker("runner.worker.js");
    state.runner = runner;
    state.runnerTimer = window.setTimeout(() => {
      runner.terminate();
      state.runner = null;
      elements.stderr.textContent = t("program.timedOut", {
        seconds: RUN_TIMEOUT_MS / 1000,
      });
      setRunMetaSuffix({ key: "meta.timedOut" });
      setStatus("status.timedOut");
      resolve();
    }, RUN_TIMEOUT_MS);

    runner.addEventListener("message", (event: MessageEvent<RunnerMessage>) => {
      const message = event.data;
      window.clearTimeout(state.runnerTimer);
      runner.terminate();
      state.runner = null;
      if (message.type === "result") {
        elements.stdout.textContent = message.stdout;
        elements.stderr.textContent = message.stderr;
        setRunMetaSuffix({ key: "meta.exitCode", params: { code: message.exitCode } });
        if (message.exitCode === 0) {
          setStatus("status.runFinished");
        } else {
          setStatus("status.exited", { code: message.exitCode });
        }
      } else {
        elements.stderr.textContent = message.message;
        setRunMetaSuffix({ key: "meta.notRunnable" });
        setStatus("status.runFailed");
      }
      resolve();
    });

    runner.addEventListener("error", (event) => {
      window.clearTimeout(state.runnerTimer);
      elements.stderr.textContent = event.message || "the run worker stopped";
      setStatus("status.runFailed");
      resolve();
    });

    runner.postMessage({ type: "run", wasm: wasmBuffer }, [wasmBuffer]);
  });
}

export async function run(): Promise<void> {
  if (state.running) {
    return;
  }
  setRunning(true);
  showTab("output");
  setRunMeta({ key: "meta.compiling" });
  elements.stdout.textContent = "";
  elements.stderr.textContent = "";
  setStatus("status.compiling");

  try {
    const started = performance.now();
    const response = await callCompiler("compile", normalizedSource());
    if (response.error !== undefined) {
      setRunMeta({ key: "meta.compileFailed" });
      setStatus("status.compilerUnavailable");
      return;
    }
    renderDiagnostics(response.diagnostics);
    if (!response.ok || response.wasm === null) {
      showTab("problems");
      setRunMeta({ key: "meta.compileFailed" });
      setStatus("status.compileFailed");
      return;
    }
    const ms = (performance.now() - started).toFixed(1);
    const bytes = response.wasm.byteLength;
    setRunMeta({
      key: "meta.compiled",
      params: { ms, bytes, functions: response.stats?.functionCount ?? 0 },
    });
    setStatus("status.running");
    await execute(response.wasm);
  } catch (error) {
    showTab("problems");
    renderDiagnostics([
      {
        severity: "error",
        code: null,
        message: error instanceof Error ? error.message : String(error),
        span: null,
        labels: [],
      },
    ]);
    setStatus("status.compilerError");
  } finally {
    setRunning(false);
  }
}
