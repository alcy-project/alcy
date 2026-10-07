// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The site's shared behavior: the color theme and the page's language.
//
// The language comes from the tree the page was generated into
// (`document.documentElement.lang`); nothing switches it at runtime,
// because the generator already stamped every label. The theme is a
// stored preference (`alcy-site-theme`) that `boot.js` applies to
// `data-theme` before first paint; the settings menu's toggle drives it
// afterwards.

import { toLanguage, translate } from "./i18n.js";
import type { Language, MessageKey, MessageParams } from "./i18n.js";

const THEME_KEY = "alcy-site-theme";
const LEGACY_THEME_KEY = "alcy-playground-theme";
const THEME_MODES = ["auto", "light", "dark"] as const;
type ThemeMode = (typeof THEME_MODES)[number];

const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");

const language: Language = toLanguage(document.documentElement.lang);

export function t(key: MessageKey, params?: MessageParams): string {
  return translate(language, key, params);
}

function isThemeMode(value: string | null): value is ThemeMode {
  return value !== null && (THEME_MODES as readonly string[]).includes(value);
}

function storedTheme(): ThemeMode {
  try {
    const value =
      localStorage.getItem(THEME_KEY) ?? localStorage.getItem(LEGACY_THEME_KEY);
    return isThemeMode(value) ? value : "auto";
  } catch {
    return "auto";
  }
}

function persistTheme(mode: ThemeMode): void {
  try {
    localStorage.setItem(THEME_KEY, mode);
  } catch {
    // A denied store costs the preference, not the theme.
  }
}

function applyTheme(mode: ThemeMode): void {
  const dark = mode === "dark" || (mode === "auto" && darkQuery.matches);
  document.documentElement.dataset.theme = dark ? "dark" : "light";
}

const THEME_LABEL: Record<ThemeMode, MessageKey> = {
  auto: "theme.auto",
  light: "theme.light",
  dark: "theme.dark",
};

export function initThemeToggle(button: HTMLButtonElement): void {
  let mode = storedTheme();

  const render = (): void => {
    button.dataset.themeMode = mode;
    button.setAttribute("aria-label", t(THEME_LABEL[mode]));
  };

  applyTheme(mode);
  render();
  button.addEventListener("click", () => {
    const next = THEME_MODES[(THEME_MODES.indexOf(mode) + 1) % THEME_MODES.length];
    mode = next ?? "auto";
    persistTheme(mode);
    applyTheme(mode);
    render();
  });
  darkQuery.addEventListener("change", () => {
    if (mode === "auto") {
      applyTheme(mode);
    }
  });
}
