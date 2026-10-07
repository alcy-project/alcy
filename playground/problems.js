// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The diagnostics list. Diagnostics arrive as the compiler's JSON array;
// this module renders them, keeps the last set for a language switch, and
// maps a click back to the source range.

import { elements } from "./elements.js";
import { syncScroll } from "./editor.js";
import { t } from "./language.js";
import { state } from "./state.js";
import { byteToUtf16Map, utf16IndexAtByte } from "./textutil.js";

function lineColumnAt(text, index) {
  let line = 1;
  let lineStart = 0;
  for (let i = 0; i < index; i++) {
    if (text.charCodeAt(i) === 10) {
      line++;
      lineStart = i + 1;
    }
  }
  return { line, column: index - lineStart + 1 };
}

function selectionRangeFor(span) {
  const text = elements.source.value;
  const table = byteToUtf16Map(text);
  const start = utf16IndexAtByte(table, span.offset);
  const end = utf16IndexAtByte(table, span.offset + span.length);
  return { start, end };
}

function selectSpan(span) {
  const { start, end } = selectionRangeFor(span);
  const text = elements.source.value;
  const { line } = lineColumnAt(text, start);
  const lineHeight = Number.parseFloat(getComputedStyle(elements.source).lineHeight) || 20;
  const firstVisible = elements.source.scrollTop / lineHeight;
  const visibleLines = elements.source.clientHeight / lineHeight;
  elements.source.focus();
  elements.source.setSelectionRange(start, end);
  if (line < firstVisible + 2 || line > firstVisible + visibleLines - 2) {
    elements.source.scrollTop = Math.max(0, (line - 3) * lineHeight);
  }
  syncScroll();
}

function codeLabel(code) {
  if (code === null || code === undefined) {
    return "";
  }
  return `${code.stage} ${code.local_id}`;
}

// The badge says how loud the loudest diagnostic is rather than always
// reading as an error.
function worstSeverity(diagnostics) {
  let worst = "note";
  for (const diagnostic of diagnostics) {
    const severity = diagnostic.severity ?? "error";
    if (severity === "error") {
      return "error";
    }
    if (severity === "warning") {
      worst = "warning";
    }
  }
  return worst;
}

export function renderDiagnostics(diagnostics) {
  state.lastDiagnostics = diagnostics;
  state.compilerErrorShown = false;
  elements.problems.replaceChildren();
  elements.problemsEmpty.hidden = diagnostics.length > 0;
  elements.problemsCount.hidden = diagnostics.length === 0;
  elements.problemsCount.textContent = String(diagnostics.length);
  elements.problemsCount.dataset.severity = worstSeverity(diagnostics);

  const text = elements.source.value;
  const table =
    diagnostics.some((diagnostic) => diagnostic.span !== null)
      ? byteToUtf16Map(text)
      : null;

  for (const diagnostic of diagnostics) {
    const item = document.createElement("li");
    const severityName = diagnostic.severity ?? "error";
    item.className =
      severityName === "warning" || severityName === "note" ? severityName : "error";

    const head = document.createElement("div");
    head.className = "problem-head";

    const severity = document.createElement("span");
    severity.className = `severity ${severityName}`;
    severity.textContent = t(`severity.${severityName}`);
    head.append(severity);

    if (diagnostic.span) {
      const start = utf16IndexAtByte(table, diagnostic.span.offset);
      const { line, column } = lineColumnAt(text, start);
      const location = document.createElement("span");
      location.className = "problem-location";
      location.textContent = `${diagnostic.span.file ?? "source"}:${line}:${column}`;
      head.append(location);
    }

    const label = codeLabel(diagnostic.code);
    if (label !== "") {
      const code = document.createElement("span");
      code.className = "problem-code";
      code.textContent = label;
      head.append(code);
    }

    const message = document.createElement("div");
    message.className = "problem-message";
    message.textContent = diagnostic.message ?? "";

    item.append(head, message);
    if (diagnostic.span) {
      item.addEventListener("click", () => selectSpan(diagnostic.span));
    }
    elements.problems.append(item);
  }
}

// Re-renders what is on screen, for a language switch.
export function renderLast() {
  renderDiagnostics(state.lastDiagnostics);
}
