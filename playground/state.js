// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The page's mutable state, shared by the feature modules. Everything a
// module needs to know about another module's work is a field here, so a
// reader can see the whole surface in one place.

export const state = {
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
  // The splitter sets this from its stored value when it initializes.
  sideFraction: 0,
  codeSaveTimer: null,
  checkTimer: null,
  running: false,
  runner: null,
  runnerTimer: null,
  language: "en",
  status: { key: "status.ready" },
  runMeta: null,
  lastDiagnostics: [],
  compilerErrorShown: false,
};
