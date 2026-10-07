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
// with a data attribute.
const MENU_ICON = `<svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M3 6h18M3 12h18M3 18h18"/></svg>`;
const AUTO_ICON = `<svg class="theme-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 1 0 18z" fill="currentColor" stroke="none"/></svg>`;
const LIGHT_ICON = `<svg class="theme-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4"/></svg>`;
const DARK_ICON = `<svg class="theme-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>`;

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
