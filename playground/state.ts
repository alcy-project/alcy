// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The page's mutable state, shared by the feature modules. Everything a
// module needs to know about another module's work is a field here, so a
// reader can see the whole surface in one place.

import type { MessageKey, MessageParams, Language } from "./i18n.js";
import type {
  CompilerResponse,
  CompilerStatusName,
  Diagnostic,
  RunMeta,
} from "./types.js";

export interface Highlighter {
  highlight(text: string): string;
  dispose(): void;
}

export interface PendingRequest {
  resolve: (response: CompilerResponse) => void;
  reject: (error: Error) => void;
}

export interface StatusRef {
  key: MessageKey;
  params?: MessageParams;
}

export interface PageState {
  highlighter: Highlighter | null;
  compiler: Worker | null;
  compilerStatus: CompilerStatusName;
  compilerDetail: string;
  requests: Map<number, PendingRequest>;
  requestSeq: number;
  checkSeq: number;
  highlightFrame: number | null;
  highlightedText: string | null;
  gutterLines: number;
  // The splitter sets this from its stored value when it initializes.
  sideFraction: number;
  codeSaveTimer: number | undefined;
  checkTimer: number | undefined;
  running: boolean;
  runner: Worker | null;
  runnerTimer: number | undefined;
  language: Language;
  status: StatusRef;
  runMeta: RunMeta | null;
  lastDiagnostics: Diagnostic[];
  compilerErrorShown: boolean;
}

export const state: PageState = {
  highlighter: null,
  compiler: null,
  compilerStatus: "idle",
  compilerDetail: "",
  requests: new Map(),
  requestSeq: 0,
  checkSeq: 0,
  highlightFrame: null,
  highlightedText: null,
  gutterLines: 0,
  sideFraction: 0,
  codeSaveTimer: undefined,
  checkTimer: undefined,
  running: false,
  runner: null,
  runnerTimer: undefined,
  language: "en",
  status: { key: "status.ready" },
  runMeta: null,
  lastDiagnostics: [],
  compilerErrorShown: false,
};
