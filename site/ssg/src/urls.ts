// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The deployed site's address, for the few things that must be absolute:
// canonical links, the alternates between language trees, and the
// sitemap. GitHub Pages serves the project under `/alcy/`.

import { LANGUAGES } from "../../shared/i18n.js";
import { prefixOf } from "./languages.js";
import type { Language } from "./languages.js";

export const SITE_URL = "https://alcy-project.github.io/alcy/";

// Where that address sits on the server, for the 404 page's base.
export const SITE_PATH = SITE_URL.replace(/^[a-z]+:\/\/[^/]+/, "");

// A directory index is served at its directory: `guide/index.html` is
// reached as `guide/`.
export function canonicalUrl(route: string): string {
  const suffix = "index.html";
  const trimmed = route.endsWith(suffix) ? route.slice(0, -suffix.length) : route;
  return SITE_URL + trimmed;
}

// Every page names itself and the same page in the other trees.
export function pageLinks(language: Language, treePath: string): string {
  const lines = [
    `<link rel="canonical" href="${canonicalUrl(prefixOf(language) + treePath)}" />`,
  ];
  for (const other of LANGUAGES) {
    lines.push(
      `<link rel="alternate" hreflang="${other}" href="${canonicalUrl(prefixOf(other) + treePath)}" />`,
    );
  }
  return lines.join("\n    ");
}
