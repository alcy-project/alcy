// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The link audit. Every link a guide page renders goes through `resolve`:
// a page-to-page `.md` link becomes the built `.html`, a link to anything
// else in the repository becomes a GitHub URL, and a link into the built
// site must point at something that exists. A target that resolves
// nowhere is an error, so the build fails rather than publishing a dead
// link.

import { posix } from "node:path";

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
const GUIDE_PREFIX = "docs/guide/";

function splitTarget(url: string): { path: string; suffix: string } {
  const cut = url.search(/[?#]/);
  return cut === -1
    ? { path: url, suffix: "" }
    : { path: url.slice(0, cut), suffix: url.slice(cut) };
}

export function createLinkChecker(context: LinkContext): LinkChecker {
  const pagesBySource = new Map(context.pages.map((page) => [page.sourceRel, page]));
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
        const repoPath = posix.normalize(
          posix.join("docs/guide", posix.dirname(page.sourceRel), path),
        );
        if (repoPath.startsWith(GUIDE_PREFIX)) {
          const sourceRel = repoPath.slice(GUIDE_PREFIX.length);
          const target = pagesBySource.get(sourceRel);
          if (target === undefined) {
            fail(page, url, `does not name a guide page (${sourceRel})`);
            return url;
          }
          return posix.relative(posix.dirname(page.route), target.route) + suffix;
        }
        if (!context.repoExists(repoPath)) {
          fail(page, url, `does not exist in the repository (${repoPath})`);
          return url;
        }
        return `${REPO_URL}/${repoPath}${suffix}`;
      }

      // A link that starts with `/` names a path from the built site's
      // root; the page's own depth decides what the reader's href has to
      // be, so the generator rebases it.
      if (path.startsWith("/")) {
        const candidate = path.slice(1);
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
