// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The site-wide preferences: color theme and language. Every page loads
// this module, so a choice made on one page holds on the next.
//
// The theme is written to `data-theme` on the root before first paint by
// `boot.js`; the resolved values here and its keys must stay in step.

import { resolveLanguage, translate } from "./i18n.js";
import type { Language, MessageKey, MessageParams } from "./i18n.js";

const THEME_KEY = "alcy-site-theme";
const LEGACY_THEME_KEY = "alcy-playground-theme";
const LANGUAGE_KEY = "alcy-site-language";
const LEGACY_LANGUAGE_KEY = "alcy-playground-language";

/** Dispatched on `document` after the language changes. */
export const LANGUAGE_CHANGED = "site-language-changed";

const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");

let language: Language = "en";

// The site's keys are new; the playground's old ones are read once and
// rewritten so a returning visitor keeps the choice they made.
function stored(key: string, legacyKey: string): string | null {
  try {
    const value = localStorage.getItem(key);
    if (value !== null) {
      return value;
    }
    const legacy = localStorage.getItem(legacyKey);
    if (legacy !== null) {
      try {
        localStorage.setItem(key, legacy);
      } catch {
        // The preference still applies; only its persistence is lost.
      }
    }
    return legacy;
  } catch {
    return null;
  }
}

function persist(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    // A denied store costs the preference, not the setting.
  }
}

export function storedTheme(): string {
  return stored(THEME_KEY, LEGACY_THEME_KEY) ?? "auto";
}

export function applyTheme(mode: string): void {
  const dark = mode === "dark" || (mode === "auto" && darkQuery.matches);
  document.documentElement.dataset.theme = dark ? "dark" : "light";
}

// `select` is the picker; every page's shell carries one.
export function initTheme(select: HTMLSelectElement): void {
  select.value = storedTheme();
  applyTheme(select.value);
  select.addEventListener("change", () => {
    persist(THEME_KEY, select.value);
    applyTheme(select.value);
  });
  darkQuery.addEventListener("change", () => {
    if (storedTheme() === "auto") {
      applyTheme("auto");
    }
  });
}

export function t(key: MessageKey, params?: MessageParams): string {
  return translate(language, key, params);
}

export function storedLanguage(): string {
  return stored(LANGUAGE_KEY, LEGACY_LANGUAGE_KEY) ?? "auto";
}

function applyI18n(): void {
  document.documentElement.lang = language;
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
  language = resolveLanguage(preference);
  applyI18n();
  return language;
}

export function initLanguage(select: HTMLSelectElement): void {
  const preference = storedLanguage();
  select.value = preference === "auto" ? "auto" : resolveLanguage(preference);
  applyLanguage(preference);
  select.addEventListener("change", () => {
    persist(LANGUAGE_KEY, select.value);
    applyLanguage(select.value);
    document.dispatchEvent(new CustomEvent(LANGUAGE_CHANGED));
  });
}
