// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The chrome that reports what the page is doing: the status line, the
// compiler's availability, and the Run button's busy state. Messages are
// kept as catalog keys so switching the language re-renders them.

import { elements } from "./elements.js";
import type { MessageKey, MessageParams } from "../shared/i18n.js";
import { t } from "../shared/site.js";
import { renderDiagnostics } from "./problems.js";
import { state } from "./state.js";
import { showTab } from "./tabs.js";
import type { CompilerStatusName, Diagnostic } from "./types.js";

export function renderStatus(): void {
  elements.status.textContent = t(state.status.key, state.status.params);
}

export function setStatus(key: MessageKey, params?: MessageParams): void {
  state.status = { key, params };
  renderStatus();
}

export function renderCompilerState(): void {
  elements.compilerState.dataset.state = state.compilerStatus;
  elements.compilerStateText.textContent = t(`compiler.${state.compilerStatus}`);
  elements.compilerState.title = state.compilerDetail;
}

export function setCompilerState(status: CompilerStatusName, detail = ""): void {
  state.compilerStatus = status;
  state.compilerDetail = detail;
  renderCompilerState();
}

export function showCompilerError(detail: string): void {
  const error: Diagnostic = {
    severity: "error",
    code: null,
    message: t("compiler.missing", { detail: detail ? `: ${detail}` : "" }),
    span: null,
    labels: [],
  };
  state.lastDiagnostics = [error];
  renderDiagnostics(state.lastDiagnostics);
  // The message is this module's own text, so a language switch has to
  // re-render it; a diagnostic from the compiler is kept verbatim.
  state.compilerErrorShown = true;
  showTab("problems");
  setStatus("status.compilerUnavailable");
}

export function renderRunButton(): void {
  elements.run.setAttribute(
    "aria-label",
    t(state.running ? "button.running" : "button.run"),
  );
}

export function setRunning(running: boolean): void {
  state.running = running;
  elements.run.disabled = running;
  elements.check.disabled = running;
  elements.run.classList.toggle("busy", running);
  renderRunButton();
}
