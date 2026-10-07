// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The editor's buffer and the sample it came from live in localStorage, so
// a reload returns to the code that was being worked on rather than to the
// default example. Writes are debounced; `pagehide` flushes the last one.

import { elements } from "./elements.js";
import { state } from "./state.js";

const CODE_KEY = "alcy-playground-code";
const CODE_SAVE_DEBOUNCE_MS = 300;

export interface SavedCode {
  source: string;
  sampleFile: string | null;
}

export function readSavedCode(): SavedCode | null {
  try {
    const raw = localStorage.getItem(CODE_KEY);
    if (raw === null) {
      return null;
    }
    const saved: unknown = JSON.parse(raw);
    if (
      typeof saved !== "object" ||
      saved === null ||
      typeof (saved as { source?: unknown }).source !== "string"
    ) {
      return null;
    }
    const source = (saved as { source: string }).source;
    const sampleFile = (saved as { sampleFile?: unknown }).sampleFile;
    return {
      source,
      sampleFile: typeof sampleFile === "string" ? sampleFile : null,
    };
  } catch {
    return null;
  }
}

export function saveCodeNow(): void {
  window.clearTimeout(state.codeSaveTimer);
  state.codeSaveTimer = undefined;
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

export function scheduleCodeSave(): void {
  window.clearTimeout(state.codeSaveTimer);
  state.codeSaveTimer = window.setTimeout(saveCodeNow, CODE_SAVE_DEBOUNCE_MS);
}
