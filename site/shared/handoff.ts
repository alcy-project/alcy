// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The handoff from the guide to the playground: a block's text travels
// in sessionStorage, so the reader's tab carries it over and the store
// dies with the tab. Every tree's pages share the one origin store.

export const HANDOFF_KEY = "alcy-playground-handoff";

// A denied or full store costs the handoff, not the navigation.
export function handOffToPlayground(source: string): boolean {
  try {
    sessionStorage.setItem(HANDOFF_KEY, JSON.stringify({ source }));
    return true;
  } catch {
    return false;
  }
}

export function takeHandoff(): string | null {
  try {
    const raw = sessionStorage.getItem(HANDOFF_KEY);
    if (raw === null) {
      return null;
    }
    sessionStorage.removeItem(HANDOFF_KEY);
    const handed: unknown = JSON.parse(raw);
    const source = (handed as { source?: unknown } | null)?.source;
    return typeof source === "string" && source !== "" ? source : null;
  } catch {
    return null;
  }
}
