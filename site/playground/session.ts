// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The editor's buffer and the sample it came from live in localStorage, so
// a reload returns to the code that was being worked on rather than to the
// default example. Writes are debounced; `pagehide` flushes the last one.

import { elements } from "./elements.js";
import { takeHandoff } from "../shared/handoff.js";
import { state } from "./state.js";

const CODE_KEY = "alcy-playground-code";
const CODE_SAVE_DEBOUNCE_MS = 300;
const LINK_PREFIX = "#code=";

// A program can arrive from outside the saved session: a guide block
// hands its text over through sessionStorage, and a shared link carries
// it in the URL hash. The link is the more deliberate act, so it
// outranks the handoff; both outrank the saved buffer.

export interface IncomingSource {
  source: string;
  origin: "link" | "handoff";
}

function decodeLink(part: string): string | null {
  try {
    const source = new TextDecoder().decode(
      Uint8Array.from(atob(part), (char: string) => char.charCodeAt(0)),
    );
    return source === "" ? null : source;
  } catch {
    return null;
  }
}

export function takeIncomingSource(): IncomingSource | null {
  if (window.location.hash.startsWith(LINK_PREFIX)) {
    const source = decodeLink(window.location.hash.slice(LINK_PREFIX.length));
    if (source !== null) {
      return { source, origin: "link" };
    }
  }
  const handed = takeHandoff();
  return handed === null ? null : { source: handed, origin: "handoff" };
}

// The link was read; a reload now starts from the buffer, not from a
// stale copy of the same link.
export function clearLinkHash(): void {
  if (window.location.hash !== "") {
    window.history.replaceState(
      null,
      "",
      window.location.pathname + window.location.search,
    );
  }
}

// The shareable form of the buffer: base64 of its UTF-8 bytes, small
// enough for the URLs a browser carries without complaint.
export function shareLink(source: string): string {
  const bytes = new TextEncoder().encode(source);
  let binary = "";
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return `${window.location.origin}${window.location.pathname}${LINK_PREFIX}${btoa(binary)}`;
}

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
