// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Searching the guide. The build writes one index of the pages and
// their sections; the reader's own tree decides where a result links,
// so one index serves every language. The dialog and the index both
// load on the first search -- a reader who never asks pays for
// nothing, and the slash and Ctrl/Cmd-K keys reach the same place as
// the header's button.

import { t } from "./site.js";

interface Section {
  id: string;
  text: string;
}

interface Entry {
  route: string;
  title: string;
  sections: Section[];
}

interface Result {
  route: string;
  title: string;
  section: Section | null;
}

const RESULT_LIMIT = 12;
const INDEX_URL = new URL("../search-index.json", import.meta.url);

let entries: Entry[] | null = null;
let dialog: HTMLDialogElement | null = null;
let input: HTMLInputElement | null = null;
let list: HTMLOListElement | null = null;

async function loadEntries(): Promise<Entry[]> {
  if (entries === null) {
    try {
      const response = await fetch(INDEX_URL);
      const parsed: unknown = await response.json();
      entries = Array.isArray(parsed) ? (parsed as Entry[]) : [];
    } catch {
      entries = [];
    }
  }
  return entries;
}

// A result's route is relative to a tree's root, and the header's own
// brand link already knows where this page sits in its tree: it is the
// climb to the root the reader is reading, whatever the deployment put
// in front of it.
function resultHref(route: string, hash: string): string {
  const brand = document.querySelector<HTMLAnchorElement>(".site-brand");
  const root = brand === null ? "./" : (brand.getAttribute("href") ?? "./");
  const base = root.endsWith("/") ? root : `${root}/`;
  return `${base}${route}${hash}`;
}

function matches(entry: Entry, query: string): Result[] {
  if (entry.title.toLowerCase().includes(query)) {
    return [{ route: entry.route, title: entry.title, section: null }];
  }
  return entry.sections
    .filter((section) => section.text.toLowerCase().includes(query))
    .map((section) => ({ route: entry.route, title: entry.title, section }));
}

function queryIndex(loaded: readonly Entry[], query: string): Result[] {
  const results: Result[] = [];
  for (const entry of loaded) {
    for (const result of matches(entry, query)) {
      results.push(result);
      if (results.length >= RESULT_LIMIT) {
        return results;
      }
    }
  }
  return results;
}

function renderResults(results: readonly Result[]): void {
  if (list === null) {
    return;
  }
  const items = [];
  if (results.length === 0) {
    const empty = document.createElement("li");
    empty.className = "site-search-none";
    empty.textContent = t("search.none");
    items.push(empty);
  }
  for (const result of results) {
    const item = document.createElement("li");
    const link = document.createElement("a");
    const hash = result.section === null ? "" : `#${result.section.id}`;
    link.href = resultHref(result.route, hash);
    link.textContent =
      result.section === null
        ? result.title
        : `${result.title} — ${result.section.text}`;
    // A result on this page is an anchor: the dialog has to get out of
    // the way before the jump happens.
    link.addEventListener("click", () => {
      dialog?.close();
    });
    item.append(link);
    items.push(item);
  }
  list.replaceChildren(...items);
}

function runSearch(): void {
  const query = (input?.value ?? "").trim().toLowerCase();
  if (query === "") {
    list?.replaceChildren();
    return;
  }
  void loadEntries().then((loaded) => {
    if (input === null || input.value.trim().toLowerCase() !== query) {
      return;
    }
    renderResults(queryIndex(loaded, query));
  });
}

function buildDialog(): HTMLDialogElement {
  const created = document.createElement("dialog");
  created.className = "site-search";
  const field = document.createElement("input");
  field.type = "search";
  field.placeholder = t("search.placeholder");
  field.setAttribute("aria-label", t("search.placeholder"));
  field.addEventListener("input", runSearch);
  field.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      list?.querySelector<HTMLAnchorElement>("a")?.click();
    }
  });
  const results = document.createElement("ol");
  created.append(field, results);
  created.addEventListener("click", (event) => {
    // A click that reaches the dialog itself is the backdrop's.
    if (event.target === created) {
      created.close();
    }
  });
  document.body.append(created);
  input = field;
  list = results;
  return created;
}

function openSearch(): void {
  const opened = dialog ?? buildDialog();
  dialog = opened;
  opened.showModal();
  input?.focus();
  input?.select();
  void loadEntries();
}

function opensSearch(event: KeyboardEvent): boolean {
  const target = event.target as HTMLElement | null;
  if (
    target !== null &&
    (target.tagName === "INPUT" ||
      target.tagName === "TEXTAREA" ||
      target.tagName === "SELECT" ||
      target.isContentEditable)
  ) {
    return false;
  }
  if (event.key === "/" && !event.ctrlKey && !event.metaKey) {
    return true;
  }
  return (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k";
}

export function initSearch(): void {
  const opener = document.querySelector<HTMLButtonElement>('[data-action="search"]');
  if (opener === null) {
    return;
  }
  opener.addEventListener("click", openSearch);
  document.addEventListener("keydown", (event) => {
    if (!event.defaultPrevented && opensSearch(event)) {
      event.preventDefault();
      openSearch();
    }
  });
}
