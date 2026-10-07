// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Light, dark, or the system's choice. The resolved theme is written to
// `data-theme` on the root, which is what style.css keys on.

import { elements } from "./elements.js";

const THEME_KEY = "alcy-playground-theme";
const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");

function storedTheme(): string {
  try {
    return localStorage.getItem(THEME_KEY) ?? "auto";
  } catch {
    return "auto";
  }
}

export function applyTheme(mode: string): void {
  const dark = mode === "dark" || (mode === "auto" && darkQuery.matches);
  document.documentElement.dataset.theme = dark ? "dark" : "light";
  elements.theme.value = mode;
}

export function initTheme(): void {
  applyTheme(storedTheme());
  elements.theme.addEventListener("change", () => {
    const mode = elements.theme.value;
    try {
      localStorage.setItem(THEME_KEY, mode);
    } catch {
      // A denied store costs the preference, not the theme.
    }
    applyTheme(mode);
  });
  darkQuery.addEventListener("change", () => {
    if (storedTheme() === "auto") {
      applyTheme("auto");
    }
  });
}
