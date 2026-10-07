// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The page's language: the stored preference, the catalogs, and the
// `data-i18n` pass over the markup. Rendering the pieces that keep their
// own state (status line, diagnostics, run meta) is the caller's job, so
// this module stays a leaf.

import { elements } from "./elements.js";
import { resolveLanguage, translate } from "./i18n.js";
import type { Language, MessageKey, MessageParams } from "./i18n.js";
import { state } from "./state.js";

const LANGUAGE_KEY = "alcy-playground-language";

export function t(key: MessageKey, params?: MessageParams): string {
  return translate(state.language, key, params);
}

export function storedLanguage(): string {
  try {
    return localStorage.getItem(LANGUAGE_KEY) ?? "auto";
  } catch {
    return "auto";
  }
}

export function persistLanguage(preference: string): void {
  try {
    localStorage.setItem(LANGUAGE_KEY, preference);
  } catch {
    // A denied store costs the preference, not the language.
  }
}

function applyI18n(): void {
  document.documentElement.lang = state.language;
  for (const node of document.querySelectorAll<HTMLElement>("[data-i18n]")) {
    node.textContent = t(node.dataset.i18n as MessageKey);
  }
  for (const node of document.querySelectorAll<HTMLElement>("[data-i18n-aria]")) {
    node.setAttribute("aria-label", t(node.dataset.i18nAria as MessageKey));
  }
  for (const node of document.querySelectorAll<HTMLElement>("[data-i18n-title]")) {
    node.title = t(node.dataset.i18nTitle as MessageKey);
  }
}

export function applyLanguage(preference: string): Language {
  state.language = resolveLanguage(preference);
  elements.language.value = preference === "auto" ? "auto" : state.language;
  applyI18n();
  return state.language;
}
