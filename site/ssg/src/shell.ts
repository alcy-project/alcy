// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The shell's settings menu: one markup string for every page, built in
// the page's language. The menu holds the theme toggle and the link to
// the same page in another language; the toggle's icons switch on
// `data-theme-mode`, which `shared/site.ts` owns.

import { LANGUAGE_NAMES, translate } from "../../shared/i18n.js";
import type { Language, MessageKey } from "../../shared/i18n.js";
import { otherLanguages } from "./languages.js";

// The icons are inline so the shell needs no asset and can switch them
// with a data attribute. Each carries its own paint: a path with no fill
// is invisible, so every line is stroked in `currentColor`.
const MENU_ICON = `<svg viewBox="0 0 24 24" aria-hidden="true" focusable="false" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M3.5 6.5h17M3.5 12h17M3.5 17.5h17"/></svg>`;
const AUTO_ICON = `<svg class="theme-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="12" cy="12" r="8.5"/><path d="M12 3.5a8.5 8.5 0 0 1 0 17z" fill="currentColor" stroke="none"/></svg>`;
const LIGHT_ICON = `<svg class="theme-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.2 5.2l1.4 1.4M17.4 17.4l1.4 1.4M18.8 5.2l-1.4 1.4M6.6 17.4l-1.4 1.4"/></svg>`;
const DARK_ICON = `<svg class="theme-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>`;

function escapeAttribute(text: string): string {
  return text.replaceAll("&", "&amp;").replaceAll('"', "&quot;").replaceAll("<", "&lt;");
}

export interface SettingsOptions {
  language: Language;
  // The URL of this page in the other language, as the page's reader
  // should see it (a relative href).
  switchHref: string;
}

export function renderSettings({ language, switchHref }: SettingsOptions): string {
  const t = (key: MessageKey): string => translate(language, key);
  const other = otherLanguages(language)[0];
  const languageLink =
    other === undefined
      ? ""
      : `<a class="language-link" href="${escapeAttribute(switchHref)}" hreflang="${other}" lang="${other}">${LANGUAGE_NAMES[other]}</a>`;
  return `<details class="settings">
        <summary aria-label="${escapeAttribute(t("label.settings"))}" title="${escapeAttribute(t("label.settings"))}">${MENU_ICON}</summary>
        <div class="settings-panel">
          <div class="settings-row">
            <span class="settings-label">${t("label.theme")}</span>
            <button id="theme" type="button" class="theme-toggle" data-theme-mode="auto" aria-label="${escapeAttribute(t("theme.auto"))}">
              ${AUTO_ICON}${LIGHT_ICON}${DARK_ICON}
            </button>
          </div>
          <div class="settings-row">
            <span class="settings-label">${t("label.language")}</span>
            ${languageLink}
          </div>
        </div>
      </details>`;
}
