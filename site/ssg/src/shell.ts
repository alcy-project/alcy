// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The shell's settings menu: one markup string for every page, built in
// the page's language. The menu holds the theme toggle and the link to
// the same page in another language; the toggle's icons switch on
// `data-theme-mode`, which `shared/site.ts` owns.

import { LANGUAGE_NAMES, translate } from "../../shared/i18n.js";
import type { Language, MessageKey } from "../../shared/i18n.js";
import { otherLanguages } from "./languages.js";

// The menu icon is inline because the shell must need no asset before
// first paint. The theme's three icons are painted by the stylesheet
// from the same paths: a mask carries the shape and `currentColor`
// fills it, so the page names one element instead of carrying three
// copies of the paths.
const MENU_ICON = `<svg viewBox="0 0 24 24" aria-hidden="true" focusable="false" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M3.5 6.5h17M3.5 12h17M3.5 17.5h17"/></svg>`;
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
              <span class="theme-icon" aria-hidden="true"></span>
              <span class="theme-name">${t("theme.auto")}</span>
            </button>
          </div>
          <div class="settings-row">
            <span class="settings-label">${t("label.language")}</span>
            ${languageLink}
          </div>
        </div>
      </details>`;
}
