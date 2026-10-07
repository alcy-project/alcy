// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Every element the page drives, looked up once. The ids are the contract
// between this table and `index.html`; a missing one throws here, at
// startup, rather than later in the module that uses it.

function mustGet<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (node === null) {
    throw new Error(`the page is missing #${id}`);
  }
  return node as T;
}

function mustQuery<T extends Element>(selector: string): T {
  const node = document.querySelector(selector);
  if (node === null) {
    throw new Error(`the page is missing ${selector}`);
  }
  return node as T;
}

export const elements = {
  source: mustGet<HTMLTextAreaElement>("source"),
  // The mirror's scroll container is the <pre>; the <code> inside it is
  // what receives the highlighted HTML.
  highlight: mustGet<HTMLPreElement>("highlight"),
  highlightCode: mustGet<HTMLElement>("highlight-code"),
  gutter: mustGet<HTMLElement>("gutter"),
  layout: mustQuery<HTMLElement>(".layout"),
  splitter: mustGet<HTMLElement>("splitter"),
  samples: mustGet<HTMLSelectElement>("samples"),
  theme: mustGet<HTMLButtonElement>("theme"),
  check: mustGet<HTMLButtonElement>("check"),
  run: mustGet<HTMLButtonElement>("run"),
  compilerState: mustGet<HTMLElement>("compiler-state"),
  compilerStateText: mustGet<HTMLElement>("compiler-state-text"),
  status: mustGet<HTMLElement>("status"),
  tabProblems: mustGet<HTMLButtonElement>("tab-problems"),
  tabOutput: mustGet<HTMLButtonElement>("tab-output"),
  panelProblems: mustGet<HTMLElement>("panel-problems"),
  panelOutput: mustGet<HTMLElement>("panel-output"),
  problems: mustGet<HTMLUListElement>("problems"),
  problemsEmpty: mustGet<HTMLElement>("problems-empty"),
  problemsCount: mustGet<HTMLElement>("problems-count"),
  runMeta: mustGet<HTMLElement>("run-meta"),
  stdout: mustGet<HTMLPreElement>("stdout"),
  stderr: mustGet<HTMLPreElement>("stderr"),
};
