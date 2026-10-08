// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The shapes that cross the playground's module boundaries. The worker
// protocol and the compiler's diagnostic shapes live with the shared
// client in `shared/compiler-client.ts`, because the guide speaks the
// same protocol; this module re-exports them and keeps the page's own.
// Every name is a re-export so the feature modules keep importing from
// `./types.js`.

import type { MessageKey, MessageParams } from "../shared/i18n.js";

export type {
  CompilerMessage,
  CompilerResponse,
  CompilerStats,
  CompilerStatusMessage,
  CompilerStatusName,
  Diagnostic,
  DiagnosticCode,
  DiagnosticLabel,
  RunErrorMessage,
  RunResultMessage,
  RunnerMessage,
  Severity,
  SourceSpan,
} from "../shared/compiler-client.js";

// A piece of text kept as a catalog key, so the status line and the run
// meta render in the page's language when they are drawn.
export interface TextRef {
  key: MessageKey;
  params?: MessageParams;
}

export interface RunMeta {
  main: TextRef | null;
  suffix: TextRef | null;
}
