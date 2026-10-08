// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The run side of the page: the compiler worker behind Check and the
// disposable runner worker behind Run.

import { assetBase } from "./assets.js";
import { elements } from "./elements.js";
import { normalizedSource } from "./editor.js";
import { openCompiler, runProgram } from "../shared/compiler-client.js";
import type { CompilerClient } from "../shared/compiler-client.js";
import { t } from "../shared/site.js";
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
import type { CompilerResponse, CompilerStatusName } from "./types.js";

const CHECK_DEBOUNCE_MS = 500;
const RUN_TIMEOUT_MS = 5000;

let compilerClient: CompilerClient | null = null;

// The client creates the worker on the first request and keeps it for the
// page; `applyCompilerState` hears every load status in between.
function callCompiler(
  op: "check" | "compile",
  source: string,
): Promise<CompilerResponse> {
  if (compilerClient === null) {
    compilerClient = openCompiler({ base: assetBase, onStatus: applyCompilerState });
  }
  return compilerClient.request(op, source);
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
      // The worker times the compiler call; the fallback covers a stale
      // worker that predates the field.
      const elapsed = performance.now() - started;
      const ms = (response.ms ?? elapsed).toFixed(1);
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

async function execute(wasmBuffer: ArrayBuffer): Promise<void> {
  const outcome = await runProgram({
    base: assetBase,
    wasm: wasmBuffer,
    timeoutMs: RUN_TIMEOUT_MS,
  });
  switch (outcome.type) {
    case "result": {
      elements.stdout.textContent = outcome.stdout;
      elements.stderr.textContent = outcome.stderr;
      setRunMetaSuffix({ key: "meta.exitCode", params: { code: outcome.exitCode } });
      const ms = outcome.ms.toFixed(1);
      if (outcome.exitCode === 0) {
        setStatus("status.runFinished", { ms });
      } else {
        setStatus("status.exited", { code: outcome.exitCode, ms });
      }
      return;
    }
    case "timeout":
      elements.stderr.textContent = t("program.timedOut", {
        seconds: RUN_TIMEOUT_MS / 1000,
      });
      setRunMetaSuffix({ key: "meta.timedOut" });
      setStatus("status.timedOut");
      return;
    case "error":
      elements.stderr.textContent = outcome.message;
      setRunMetaSuffix({ key: "meta.notRunnable" });
      setStatus("status.runFailed");
  }
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
    const elapsed = performance.now() - started;
    const ms = (response.ms ?? elapsed).toFixed(1);
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
