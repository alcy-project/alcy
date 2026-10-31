// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The site's service worker, served from the site root so its scope
// covers every tree. It keeps the heavy runtime assets -- the
// compiler's wasm module, the tree-sitter binding, the grammar -- warm
// across pages and visits: a reader who runs one guide example and
// follows a link never re-downloads what the page before fetched.
// Only those files are served cache-first; the site's own modules and
// stylesheets are network-first from a second cache, so a failed fetch
// still renders the page; documents and everything else pass through,
// so a page that runs nothing still downloads nothing.
//
// The version arrives two ways. The build substitutes the hash of those
// assets into `__ALCY_ASSET_VERSION__` when it writes this file, which
// is what a cache is named by, so a deploy that changes them ships a new
// service worker and the previous cache is retired on activation with no
// request of its own. `asset-version.json` is read only when that
// substitution did not happen (a plain `tsc` output), or -- on a
// navigation or an activation -- to notice a deploy whose assets changed
// while this worker was already installed.

const ASSET_NAMES = [
  "alcy_playground.js",
  "alcy_playground.wasm",
  "web-tree-sitter.js",
  "web-tree-sitter.wasm",
  "tree-sitter-alcy.wasm",
];

const CACHE_PREFIX = "alcy-assets-";
const VERSION_FILE = "asset-version.json";

// The site's own modules and stylesheets. These change with every
// deploy, so they are network-first: the copy in the cache is only
// there for the moment the network fails. Every module a page imports
// statically belongs here: one that is missing is fetched straight from
// the network, which means it is simply absent when the network is.
const SHELL_NAMES = [
  "site.css",
  "site.js",
  "elements.js",
  "search.js",
  "i18n.en.js",
  "i18n.ja.js",
  "shell.js",
  "guide.css",
  "guide-copy.js",
  "guide.js",
  "guide-run.js",
  "clipboard.js",
  "compiler-client.js",
];

// The build fills this literal in when it writes the file; an empty
// string means it did not, and the version file is read instead.
const __ALCY_ASSET_VERSION__ = "";

// The webworker library types `self` as a plain worker; the service
// worker APIs sit on this narrower interface.
const scope = self as unknown as ServiceWorkerGlobalScope;

let cacheName: Promise<string> | null = null;

// An unreadable or missing version file disables caching rather than
// serving assets whose freshness cannot be named.
async function readCacheName(): Promise<string> {
  const response = await fetch(VERSION_FILE, { cache: "no-cache" });
  if (!response.ok) {
    return "";
  }
  const version: { v?: unknown } = await response.json();
  return typeof version.v === "string" && version.v !== "" ? version.v : "";
}

// What this worker names its assets by, without a request: the build's
// substitution when it is there.
function initialCacheName(): Promise<string> {
  return __ALCY_ASSET_VERSION__ === ""
    ? readCacheName()
    : Promise.resolve(__ALCY_ASSET_VERSION__);
}

function currentCacheName(): Promise<string> {
  if (cacheName === null) {
    cacheName = initialCacheName();
  }
  return cacheName;
}

// Re-reads the version and retires every cache it no longer names.
async function refreshCacheName(): Promise<void> {
  cacheName = readCacheName();
  const keep = CACHE_PREFIX + (await cacheName);
  const names = await caches.keys();
  for (const name of names) {
    if (name.startsWith(CACHE_PREFIX) && name !== keep) {
      await caches.delete(name);
    }
  }
}

scope.addEventListener("install", () => {
  void scope.skipWaiting();
});

scope.addEventListener("activate", () => {
  void scope.clients.claim();
  void refreshCacheName();
});

scope.addEventListener("fetch", (event: FetchEvent) => {
  const request = event.request;
  if (request.method !== "GET" || new URL(request.url).origin !== scope.location.origin) {
    return;
  }
  if (request.mode === "navigate") {
    // The document itself is never cached; this is the update check.
    void refreshCacheName();
    return;
  }
  const name = request.url.slice(request.url.lastIndexOf("/") + 1);
  if (SHELL_NAMES.includes(name)) {
    event.respondWith(shell(request));
    return;
  }
  if (!ASSET_NAMES.includes(name)) {
    return;
  }
  event.respondWith(asset(request));
});

// The shell is current whenever the network answers; a network-first
// fetch costs nothing extra for a reader whose browser would have
// revalidated the file anyway, and it keeps a deploy from needing a
// second visit. The cached copy covers a failed fetch.
async function shell(request: Request): Promise<Response> {
  const version = await currentCacheName();
  if (version === "") {
    return fetch(request);
  }
  const cache = await caches.open(CACHE_PREFIX + version);
  try {
    const response = await fetch(request);
    if (response.ok) {
      void cache.put(request, response.clone());
    }
    return response;
  } catch (error) {
    const hit = await cache.match(request);
    if (hit !== undefined) {
      return hit;
    }
    throw error;
  }
}

// The heavy assets are named by the version, so the cached copy is the
// right one by construction and never needs revalidating.
async function asset(request: Request): Promise<Response> {
  const version = await currentCacheName();
  if (version === "") {
    return fetch(request);
  }
  const cache = await caches.open(CACHE_PREFIX + version);
  const hit = await cache.match(request);
  if (hit !== undefined) {
    return hit;
  }
  const response = await fetch(request);
  if (response.ok) {
    void cache.put(request, response.clone());
  }
  return response;
}
