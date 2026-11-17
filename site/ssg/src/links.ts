// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The link audit. Every link a page renders goes through `resolve`:
// a page-to-page `.md` link becomes the built `.html` in the same
// language's tree, a link to anything else in the repository becomes a
// GitHub URL, a link starting with `/` names a path from the language
// tree's root, and a link into the built site must point at something
// that exists. A target that resolves nowhere is an error, so the build
// fails rather than publishing a dead link.

import { posix } from "node:path";

import { LANGUAGES } from "../../shared/i18n.js";
import { prefixOf } from "./languages.js";
import type { GuidePage } from "./guide.js";

export interface LinkContext {
  pages: readonly GuidePage[];
  // Whether a path relative to the built site's root exists. An empty
  // path is the root index, and a path ending in `/` is a directory.
  siteExists(route: string): boolean;
  // Whether a path relative to the repository root exists.
  repoExists(path: string): boolean;
}

export interface LinkChecker {
  resolve(url: string, page: GuidePage): string;
  errors: string[];
}

const REPO_URL = "https://github.com/alcy-project/alcy/blob/main";
const EXTERNAL = /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i;

// The directories each page tree reads its pages from, keyed by the
// first segment of a page's route. A link whose resolution starts with
// the tree's prefix names a sibling page of that tree; anything else
// in the repository is a link to the blob host.
const TREES: ReadonlyMap<string, string> = new Map([
  ["guide", "docs/guide"],
  ["reference", "docs/spec"],
  ["tools", "docs/tools"],
]);

// The tree the page came from, which is also the prefix of its route
// after the language prefix. The English tree is at the root; every
// other language nests under its own tag, so `ja/guide/...` has the
// tree `guide`.
function treeOf(page: GuidePage): string {
  let { route } = page;
  for (const language of LANGUAGES) {
    const prefix = prefixOf(language);
    if (prefix !== "" && route.startsWith(prefix)) {
      route = route.slice(prefix.length);
      break;
    }
  }
  return route.split("/")[0] ?? "";
}

function splitTarget(url: string): { path: string; suffix: string } {
  const cut = url.search(/[?#]/);
  return cut === -1
    ? { path: url, suffix: "" }
    : { path: url.slice(0, cut), suffix: url.slice(cut) };
}

export function createLinkChecker(context: LinkContext): LinkChecker {
  const pagesBySource = new Map(
    context.pages.map((page) => [`${page.language}\0${treeOf(page)}\0${page.sourceRel}`, page]),
  );
  const errors: string[] = [];

  const fail = (page: GuidePage, url: string, detail: string): void => {
    errors.push(`${page.sourceRel}: [${url}] ${detail}`);
  };

  return {
    errors,

    resolve(url: string, page: GuidePage): string {
      if (url.startsWith("#") || EXTERNAL.test(url)) {
        return url;
      }
      const { path, suffix } = splitTarget(url);
      if (path === "") {
        return url;
      }

      if (path.endsWith(".md")) {
        // The page's own tree first: a sibling of the page links like a
        // page. Otherwise the path is anywhere in the repository, and a
        // repoExists check decides.
        const tree = treeOf(page);
        const root = TREES.get(tree);
        if (root !== undefined) {
          const repoPath = posix.normalize(
            posix.join(root, posix.dirname(page.sourceRel), path),
          );
          if (repoPath.startsWith(`${root}/`) || repoPath === root) {
            const stripped = repoPath.slice(root.length + 1);
            const target = stripped === "" || stripped === "."
              ? undefined
              : pagesBySource.get(`${page.language}\0${tree}\0${stripped}`);
            if (target === undefined) {
              fail(page, url, `does not name a ${tree} page (${stripped})`);
              return url;
            }
            return posix.relative(posix.dirname(page.route), target.route) + suffix;
          }
        }
        // Derive the repository path from the page's tree root, not the
        // route segment, so the two are distinct.
        const repoPath = root === undefined
          ? posix.normalize(posix.join(posix.dirname(page.sourceRel), path))
          : posix.normalize(posix.join(root, posix.dirname(page.sourceRel), path));
        if (!context.repoExists(repoPath)) {
          fail(page, url, `does not exist in the repository (${repoPath})`);
          return url;
        }
        return `${REPO_URL}/${repoPath}${suffix}`;
      }

      // A link that starts with `/` names a path from the language
      // tree's root; the page's own depth decides what the reader's href
      // has to be, so the generator rebases it.
      if (path.startsWith("/")) {
        const candidate = prefixOf(page.language) + path.slice(1);
        if (!context.siteExists(candidate)) {
          fail(page, url, `does not exist in the built site (${candidate || "index.html"})`);
          return url;
        }
        const rebased = posix.relative(posix.dirname(page.route), candidate);
        const trailing = candidate === "" || candidate.endsWith("/") ? "/" : "";
        return `${rebased}${trailing}${suffix}`;
      }

      const candidate = posix.normalize(posix.join(posix.dirname(page.route), path));
      const sitePath = candidate === "." || candidate === "./" ? "" : candidate;
      if (!context.siteExists(sitePath)) {
        fail(page, url, `does not exist in the built site (${sitePath || "index.html"})`);
      }
      return url;
    },
  };
}