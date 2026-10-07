// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The shapes that cross module boundaries: the compiler's diagnostics and
// counts, the messages the two workers exchange, and the text references
// the status line and run meta keep for a language switch.

import type { MessageKey, MessageParams } from "./i18n.js";

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
}

export interface RunErrorMessage {
  type: "error";
  message: string;
}

export type RunnerMessage = RunResultMessage | RunErrorMessage;

// A piece of text kept as a catalog key, so re-rendering in another
// language is a lookup rather than a re-run.
export interface TextRef {
  key: MessageKey;
  params?: MessageParams;
}

export interface RunMeta {
  main: TextRef | null;
  suffix: TextRef | null;
}
