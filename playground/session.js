// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The editor's buffer and the sample it came from live in localStorage, so
// a reload returns to the code that was being worked on rather than to the
// default example. Writes are debounced; `pagehide` flushes the last one.

import { elements } from "./elements.js";
import { state } from "./state.js";

const CODE_KEY = "alcy-playground-code";
const CODE_SAVE_DEBOUNCE_MS = 300;

export function readSavedCode() {
  try {
    const raw = localStorage.getItem(CODE_KEY);
    if (raw === null) {
      return null;
    }
    const saved = JSON.parse(raw);
    if (typeof saved?.source !== "string") {
      return null;
    }
    return {
      source: saved.source,
      sampleFile: typeof saved.sampleFile === "string" ? saved.sampleFile : null,
    };
  } catch {
    return null;
  }
}

export function saveCodeNow() {
  window.clearTimeout(state.codeSaveTimer);
  state.codeSaveTimer = null;
  try {
    localStorage.setItem(
      CODE_KEY,
      JSON.stringify({
        version: 1,
        source: elements.source.value,
        sampleFile: elements.samples.value || null,
      }),
    );
  } catch {
    // A full or denied store costs the snapshot, not the session.
  }
}

export function scheduleCodeSave() {
  window.clearTimeout(state.codeSaveTimer);
  state.codeSaveTimer = window.setTimeout(saveCodeNow, CODE_SAVE_DEBOUNCE_MS);
}
