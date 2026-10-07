// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Problems and Output are two views of the side panel; one is visible at a
// time and the tabs say which.

import { elements } from "./elements.js";

export function showTab(name) {
  const problems = name === "problems";
  elements.tabProblems.classList.toggle("active", problems);
  elements.tabOutput.classList.toggle("active", !problems);
  elements.tabProblems.setAttribute("aria-selected", String(problems));
  elements.tabOutput.setAttribute("aria-selected", String(!problems));
  elements.panelProblems.classList.toggle("hidden", !problems);
  elements.panelOutput.classList.toggle("hidden", problems);
}

export function initTabs() {
  elements.tabProblems.addEventListener("click", () => showTab("problems"));
  elements.tabOutput.addEventListener("click", () => showTab("output"));
}
