#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Generates the site per-language catalog modules.

The site has one tree per language; a page loads the catalog of its
own tree and nothing else. The authored source of truth is
site/shared/catalog.json; this writes one self-contained module per
language into site/shared/, compiled once per build into
shared/i18n.<lang>.js. The modules are build products and are not
tracked in git.

The tool fails the build when the data does not hold together: a
language without a name, or a key the default language lacks.
"""

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SHARED = REPO_ROOT / "site" / "shared"
CATALOG_PATH = SHARED / "catalog.json"

HEADER = """// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Generated from site/shared/catalog.json by tools/catalog.py.
// The site own strings, one module per language: the page loads
// exactly one tree set of strings, and the page is stamped by the
// generator with the same table at build time.
"""

TAIL = r"""
export type MessageKey = keyof typeof TABLENAME;
export type MessageParams = Record<string, string | number>;

export type Language = LANGTYPE;

export const LANGUAGES: Language[] = [LANGLIST];

// The tree the site serves at its root.
export const DEFAULT_LANGUAGE: Language = DEFAULTL;

// The name of a language in that language, which is what a switcher
// should show.
export const LANGUAGE_NAMES: Record<Language, string> = {LANGUAGENAMES};

// A catalog may be incomplete: a key it does not carry falls back
// to the default tree.
export type Catalog = Partial<Record<MessageKey, string>>;

const CATALOGS: Partial<Record<Language, Catalog>> = { CATALOGMEMBER };

// A page html lang names its tree; a value outside the catalog
// falls back to English so a hand-edited page still renders.
export function toLanguage(value: string): Language {
  return LANGUAGES.includes(value as Language)
    ? (value as Language)
    : DEFAULT_LANGUAGE;
}

export function translate(
  language: Language,
  key: MessageKey,
  params?: MessageParams,
): string {
  const template =
    CATALOGS[language]?.[key] ?? CATALOGS[DEFAULT_LANGUAGE]?.[key] ?? key;
  if (params === undefined) {
    return template;
  }
  return template.replace(/\{(\w+)\}/g, (match: string, name: string) =>
    name in params ? String(params[name]) : match,
  );
}

// A guard for the template stamper, so a typo in a t: key fails
// the build instead of rendering as-is.
export function isMessageKey(key: string): key is MessageKey {
  return key in (CATALOGS[DEFAULT_LANGUAGE] ?? {});
}
"""


def ts_string(value: str) -> str:
    """Emits a double-quoted TypeScript string literal."""
    out = value.replace("\\", "\\\\")
    out = out.replace('"', '\\"')
    out = out.replace("\n", "\\n")
    out = out.replace("\r", "\\r")
    out = out.replace("\t", "\\t")
    out = out.replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    return '"' + out + '"'


def validate(catalog):
    languages = catalog.get("languages")
    if not isinstance(languages, list) or not languages:
        raise SystemExit("catalog.json: languages must be a non-empty list")
    default = catalog.get("default")
    if default not in languages:
        raise SystemExit(
            f"catalog.json: default {default!r} is not in the languages list"
        )
    names = catalog.get("names") or {}
    messages = catalog.get("messages") or {}
    if not isinstance(messages.get(default), dict):
        raise SystemExit(
            f"catalog.json: no messages for the default language {default}"
        )
    for language in languages:
        if language not in names:
            raise SystemExit(f"catalog.json: no name for {language}")
        if language not in messages:
            raise SystemExit(f"catalog.json: no messages for {language}")
    default_messages = messages[default]
    for language in languages:
        if language == default:
            continue
        for key in messages[language]:
            if key not in default_messages:
                raise SystemExit(
                    f"catalog.json: the {language} message {key!r} "
                    f"has no {default} counterpart"
                )


def emit(catalog, language):
    languages = catalog["languages"]
    default = catalog["default"]
    names = catalog["names"]
    messages = catalog["messages"][language]
    lines = [HEADER, ""]
    lines.append(f"const {language} = {{")
    for key, value in messages.items():
        lines.append(f"  {ts_string(key)}: {ts_string(value)},")
    lines.append("} as const;")
    lines.append("")
    tail = TAIL
    tail = tail.replace(
        "LANGTYPE", " | ".join(ts_string(language) for language in languages)
    )
    tail = tail.replace(
        "LANGLIST", ", ".join(ts_string(language) for language in languages)
    )
    tail = tail.replace("DEFAULTL", ts_string(default))
    names_text = ", ".join(f"{name}: {ts_string(names[name])}" for name in languages)
    tail = tail.replace("LANGUAGENAMES", names_text)
    tail = tail.replace("TABLENAME", language)
    tail = tail.replace("CATALOGMEMBER", f"{language}: {language}")
    lines.append(tail)
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    if not CATALOG_PATH.is_file():
        raise SystemExit(f"catalog.json is missing at {CATALOG_PATH}")
    catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    validate(catalog)
    languages = catalog["languages"]
    for language in languages:
        target = SHARED / f"i18n.{language}.ts"
        target.write_text(emit(catalog, language), encoding="utf-8")
    sys.stdout.write("catalog: " + ", ".join(languages) + chr(10))


if __name__ == "__main__":
    main()
