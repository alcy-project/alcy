// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The landing page and the playground page are templates: the generator
// stamps one copy per language from them, so no page renders in the
// wrong language first. `{{t:key}}` pulls a catalog string (in the
// language being stamped) and `{{name}}` a value the caller computed;
// an unknown placeholder fails the build.

import { isMessageKey, translate } from "../../shared/i18n.js";
import type { Language } from "../../shared/i18n.js";
import { escapeHtml } from "../../shared/highlight.js";

export type TemplateValues = Record<string, string>;

const PLACEHOLDER = /\{\{([^{}]+)\}\}/g;

export function stampTemplate(
  source: string,
  language: Language,
  values: TemplateValues,
): string {
  return source.replace(PLACEHOLDER, (_match: string, name: string) => {
    if (name.startsWith("t:")) {
      const key = name.slice(2);
      if (!isMessageKey(key)) {
        throw new Error(`unknown message key in template: ${key}`);
      }
      return escapeHtml(translate(language, key));
    }
    const value = values[name];
    if (value === undefined) {
      throw new Error(`unknown template placeholder: ${name}`);
    }
    return value;
  });
}
