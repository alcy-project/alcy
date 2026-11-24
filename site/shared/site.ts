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
//
// The catalog arrives under the name the page's import map gives it, and
// that map names the one module for this page's tree, so a reader
// downloads one language's strings rather than the table of all of them.
// The generator writes the map into every page it emits; the
// catalog modules are generated from catalog.json at build time.

import * as catalog from "alcy:i18n";
import type { Language, MessageKey, MessageParams } from "./i18n.en.js";

const THEME_KEY = "alcy-site-theme";
const LEGACY_THEME_KEY = "alcy-playground-theme";
const THEME_MODES = ["auto", "light", "dark"] as const;
type ThemeMode = (typeof THEME_MODES)[number];

const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");

const language: Language = catalog.toLanguage(document.documentElement.lang);

export function t(key: MessageKey, params?: MessageParams): string {
  return catalog.translate(language, key, params);
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

// The settings menu and its language dropdown are details elements;
// the browser closes each only from its own summary. A press outside
// the menu closes it, and a press on the menu but outside the dropdown
// closes just the dropdown.
export function initSettingsMenu(): void {
  const menu = document.querySelector<HTMLDetailsElement>(".settings");
  if (menu === null) {
    return;
  }
  const languages = menu.querySelector<HTMLDetailsElement>(".language-menu");
  document.addEventListener("pointerdown", (event) => {
    if (!(event.target instanceof Node)) {
      return;
    }
    if (menu.open && !menu.contains(event.target)) {
      menu.open = false;
      if (languages !== null) {
        languages.open = false;
      }
      return;
    }
    if (languages !== null && languages.open &&
        !languages.contains(event.target)) {
      languages.open = false;
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && menu.open) {
      menu.open = false;
      if (languages !== null) {
        languages.open = false;
      }
      menu.querySelector<HTMLElement>("summary")?.focus();
    }
  });
}

export function initThemeToggle(button: HTMLButtonElement): void {
  let mode = storedTheme();

  const render = (): void => {
    const label = t(THEME_LABEL[mode]);
    button.dataset.themeMode = mode;
    button.setAttribute("aria-label", t("button.themeNext"));
    // The clipped name repeats the mode, so the button still says which
    // mode it holds without the stylesheet; the accessible name describes
    // the action instead.
    const name = button.querySelector(".theme-name");
    if (name !== null) {
      name.textContent = label;
    }
  };

  applyTheme(mode);
  render();
  button.addEventListener("click", () => {
    const next = THEME_MODES[(THEME_MODES.indexOf(mode) + 1) % THEME_MODES.length];
    mode = next ?? mode;
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

// Registers the service worker that keeps the compiler and tree-sitter
// assets warm across pages. The script resolves against this module's
// own URL, so every language tree reaches the one file at the site
// root; a page that cannot register -- no workers, a plain file open --
// simply runs without the warm cache.
export function registerAssetWorker(): void {
  if (!("serviceWorker" in navigator)) {
    return;
  }
  addEventListener("load", () => {
    void navigator.serviceWorker
      .register(new URL("../sw.js", import.meta.url).href)
      .catch(() => undefined);
  });
}

// Prefetch same-origin documents on hover/touch, so a link the reader is
// heading for is already warm (or finished) by the time the click lands.
// Speculation rules (injected by the generator) cover Chrome; this covers
// the rest and any browser that ignores them.
const prefetchScheduled = new WeakSet<HTMLAnchorElement>();

function isInternalAnchor(anchor: HTMLAnchorElement): boolean {
  try {
    const url = new URL(anchor.href, window.location.href);
    if (url.origin !== window.location.origin) {
      return false;
    }
    if (url.pathname === window.location.pathname && url.search === window.location.search) {
      return false;
    }
    return true;
  } catch {
    return false;
  }
}

function queuePrefetch(anchor: HTMLAnchorElement): void {
  if (prefetchScheduled.has(anchor) || !isInternalAnchor(anchor)) {
    return;
  }
  const conn = (navigator as { connection?: { saveData?: boolean } }).connection;
  if (conn?.saveData === true) {
    return;
  }
  prefetchScheduled.add(anchor);
  const link = document.createElement("link");
  link.rel = "prefetch";
  link.as = "document";
  link.href = anchor.href;
  document.head.append(link);
}

export function enableLinkPrefetch(): void {
  document.addEventListener(
    "pointerenter",
    (event) => {
      const target = event.target;
      if (!(target instanceof HTMLElement)) {
        return;
      }
      const anchor = target.closest("a[href]") as HTMLAnchorElement | null;
      if (anchor !== null) {
        queuePrefetch(anchor);
      }
    },
    { capture: true, passive: true },
  );
  document.addEventListener(
    "touchstart",
    (event) => {
      const target = event.target;
      if (!(target instanceof HTMLElement)) {
        return;
      }
      const anchor = target.closest("a[href]") as HTMLAnchorElement | null;
      if (anchor !== null) {
        queuePrefetch(anchor);
      }
    },
    { capture: true, passive: true },
  );
}
