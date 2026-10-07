// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The chrome that reports what the page is doing: the status line, the
// compiler's availability, and the Run button's busy state. Messages are
// kept as catalog keys so switching the language re-renders them.

import { elements } from "./elements.js";
import { t } from "./language.js";
import { renderDiagnostics } from "./problems.js";
import { state } from "./state.js";
import { showTab } from "./tabs.js";

export function renderStatus() {
  elements.status.textContent = t(state.status.key, state.status.params);
}

export function setStatus(key, params) {
  state.status = { key, params };
  renderStatus();
}

export function renderCompilerState() {
  elements.compilerState.dataset.state = state.compilerStatus;
  elements.compilerStateText.textContent = t(`compiler.${state.compilerStatus}`);
  elements.compilerState.title = state.compilerDetail;
}

export function setCompilerState(status, detail = "") {
  state.compilerStatus = status;
  state.compilerDetail = detail;
  renderCompilerState();
}

export function showCompilerError(detail) {
  state.lastDiagnostics = [
    {
      severity: "error",
      code: null,
      message: t("compiler.missing", { detail: detail ? `: ${detail}` : "" }),
      span: null,
      labels: [],
    },
  ];
  renderDiagnostics(state.lastDiagnostics);
  // The message is this module's own text, so a language switch has to
  // re-render it; a diagnostic from the compiler is kept verbatim.
  state.compilerErrorShown = true;
  showTab("problems");
  setStatus("status.compilerUnavailable");
}

export function renderRunButton() {
  elements.run.setAttribute(
    "aria-label",
    t(state.running ? "button.running" : "button.run"),
  );
}

export function setRunning(running) {
  state.running = running;
  elements.run.disabled = running;
  elements.check.disabled = running;
  elements.run.classList.toggle("busy", running);
  renderRunButton();
}
