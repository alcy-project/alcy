// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The Output panel: the line describing the last compile or run, and the
// program's two streams. The meta line is a translated sentence plus an
// optional suffix, kept as keys so a language switch re-renders it.

import { elements } from "./elements.js";
import { t } from "./language.js";
import { state } from "./state.js";

export function renderRunMeta() {
  if (state.runMeta === null) {
    elements.runMeta.textContent = "";
    return;
  }
  const { main, suffix } = state.runMeta;
  let text = main ? t(main.key, main.params) : "";
  if (suffix) {
    text += (text === "" ? "" : " · ") + t(suffix.key, suffix.params);
  }
  elements.runMeta.textContent = text;
}

export function setRunMeta(main) {
  state.runMeta = { main, suffix: null };
  renderRunMeta();
}

export function setRunMetaSuffix(suffix) {
  if (state.runMeta === null) {
    return;
  }
  state.runMeta.suffix = suffix;
  renderRunMeta();
}
