// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The page-side client for the playground's two workers, shared by the
// playground and the guide. `openCompiler` keeps one compiler worker
// alive for the page and turns its request/response protocol into
// promises; `runProgram` gives one compiled module a disposable worker
// and resolves with what it printed.
//
// The workers themselves live in `site/playground/`; a caller names that
// directory as `base`, so the same assets serve a page in any language
// tree. The shapes here mirror `AlcyResult` as the workers speak it.

export type Severity = "error" | "warning" | "note";

export interface SourceSpan {
  file: string | null;
  offset: number;
  length: number;
}

export interface DiagnosticCode {
  stage: string;
  local_id: number;
}

export interface DiagnosticLabel {
  span: SourceSpan | null;
  message: string;
}

export interface Diagnostic {
  severity: Severity;
  code: DiagnosticCode | null;
  message: string;
  span: SourceSpan | null;
  labels: DiagnosticLabel[];
}

export type CompilerStatusName = "idle" | "loading" | "ready" | "error";

export interface CompilerStats {
  fileCount: number;
  moduleCount: number;
  functionCount: number;
}

export interface CompilerResponse {
  type: "response";
  id: number;
  ok: boolean;
  returnCode?: number;
  error?: string;
  diagnostics: Diagnostic[];
  stats?: CompilerStats;
  wasm: ArrayBuffer | null;
  // How long the compiler call itself took, in milliseconds. The first
  // call's number does not include loading the wasm module.
  ms?: number;
}

export interface CompilerStatusMessage {
  type: "status";
  status: CompilerStatusName;
  detail: string;
}

export type CompilerMessage = CompilerResponse | CompilerStatusMessage;

export interface RunResultMessage {
  type: "result";
  stdout: string;
  stderr: string;
  exitCode: number;
  // How long the program ran, in milliseconds.
  ms: number;
}

export interface RunErrorMessage {
  type: "error";
  message: string;
}

export type RunnerMessage = RunResultMessage | RunErrorMessage;

export interface CompilerClient {
  request(op: "check" | "compile", source: string): Promise<CompilerResponse>;
  dispose(): void;
}

export interface CompilerClientOptions {
  base: URL;
  // Every status the worker reports before and while it loads; "error"
  // accompanies a rejected request.
  onStatus(status: CompilerStatusName, detail: string): void;
}

interface PendingRequest {
  resolve: (response: CompilerResponse) => void;
  reject: (error: Error) => void;
}

export function openCompiler({ base, onStatus }: CompilerClientOptions): CompilerClient {
  let worker: Worker | null = null;
  let nextId = 0;
  const pending = new Map<number, PendingRequest>();

  function failAll(message: string): void {
    for (const request of pending.values()) {
      request.reject(new Error(message));
    }
    pending.clear();
  }

  // The worker is created on the first request, not when the client is:
  // a reader who never runs anything never downloads the compiler.
  function connect(): Worker {
    if (worker !== null) {
      return worker;
    }
    const created = new Worker(new URL("compiler.worker.js", base));
    created.addEventListener("message", (event: MessageEvent<CompilerMessage>) => {
      const message = event.data;
      if (message.type === "status") {
        onStatus(message.status, message.detail);
        return;
      }
      const request = pending.get(message.id);
      if (request !== undefined) {
        pending.delete(message.id);
        request.resolve(message);
      }
    });
    created.addEventListener("error", (event) => {
      const message = event.message || "the compiler worker stopped";
      if (worker === created) {
        worker = null;
      }
      created.terminate();
      onStatus("error", message);
      failAll(message);
    });
    created.addEventListener("messageerror", () => {
      failAll("the compiler worker sent an unreadable message");
    });
    worker = created;
    return created;
  }

  return {
    request(op, source) {
      const active = connect();
      const id = ++nextId;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        active.postMessage({ type: "request", id, op, source });
      });
    },

    dispose() {
      worker?.terminate();
      worker = null;
      failAll("the compiler worker was disposed");
    },
  };
}

export interface ProgramRun {
  base: URL;
  wasm: ArrayBuffer;
  timeoutMs: number;
}

export type RunOutcome =
  | { type: "result"; stdout: string; stderr: string; exitCode: number; ms: number }
  | { type: "timeout" }
  | { type: "error"; message: string };

// The runner is disposable by design: a program that does not return is
// stopped by terminating its worker, so every run gets a fresh one.
export function runProgram({ base, wasm, timeoutMs }: ProgramRun): Promise<RunOutcome> {
  return new Promise((resolve) => {
    const worker = new Worker(new URL("runner.worker.js", base));
    let settled = false;
    const timer = window.setTimeout(() => finish({ type: "timeout" }), timeoutMs);

    function finish(outcome: RunOutcome): void {
      if (settled) {
        return;
      }
      settled = true;
      window.clearTimeout(timer);
      worker.terminate();
      resolve(outcome);
    }

    worker.addEventListener("message", (event: MessageEvent<RunnerMessage>) => {
      const message = event.data;
      if (message.type === "result") {
        finish({
          type: "result",
          stdout: message.stdout,
          stderr: message.stderr,
          exitCode: message.exitCode,
          ms: message.ms,
        });
      } else {
        finish({ type: "error", message: message.message });
      }
    });
    worker.addEventListener("error", (event) => {
      finish({ type: "error", message: event.message || "the run worker stopped" });
    });
    worker.postMessage({ type: "run", wasm }, [wasm]);
  });
}
