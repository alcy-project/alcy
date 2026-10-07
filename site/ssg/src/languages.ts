// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The URL side of the site's languages. Every page exists once per
// language; English is the tree at the site root and every other
// language lives under its own prefix (`/ja/guide/...`). Paths here are
// relative to the built site's root.

import { LANGUAGES } from "../../shared/i18n.js";
import type { Language } from "../../shared/i18n.js";

export type { Language };

export const DEFAULT_LANGUAGE: Language = "en";

export function prefixOf(language: Language): string {
  return language === DEFAULT_LANGUAGE ? "" : `${language}/`;
}

export function otherLanguages(language: Language): Language[] {
  return LANGUAGES.filter((other) => other !== language);
}
