// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Every element the page drives, looked up once. The ids are the contract
// between this table and `index.html`; a missing one surfaces when the
// module that uses it runs.

export const elements = {
  source: document.getElementById("source"),
  // The mirror's scroll container is the <pre>; the <code> inside it is
  // what receives the highlighted HTML.
  highlight: document.getElementById("highlight"),
  highlightCode: document.getElementById("highlight-code"),
  gutter: document.getElementById("gutter"),
  layout: document.querySelector(".layout"),
  splitter: document.getElementById("splitter"),
  samples: document.getElementById("samples"),
  theme: document.getElementById("theme"),
  language: document.getElementById("language"),
  check: document.getElementById("check"),
  run: document.getElementById("run"),
  compilerState: document.getElementById("compiler-state"),
  compilerStateText: document.getElementById("compiler-state-text"),
  status: document.getElementById("status"),
  tabProblems: document.getElementById("tab-problems"),
  tabOutput: document.getElementById("tab-output"),
  panelProblems: document.getElementById("panel-problems"),
  panelOutput: document.getElementById("panel-output"),
  problems: document.getElementById("problems"),
  problemsEmpty: document.getElementById("problems-empty"),
  problemsCount: document.getElementById("problems-count"),
  runMeta: document.getElementById("run-meta"),
  stdout: document.getElementById("stdout"),
  stderr: document.getElementById("stderr"),
};
