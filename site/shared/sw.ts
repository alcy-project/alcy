// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The site's service worker, served from the site root so its scope
// covers every tree. It keeps the heavy runtime assets -- the
// compiler's wasm module, the tree-sitter binding, the grammar -- warm
// across pages and visits: a reader who runs one guide example and
// follows a link never re-downloads what the page before fetched.
// Only those files are served cache-first; documents and everything
// else pass through, so a page that runs nothing still downloads
// nothing. The build stamps `asset-version.json` beside this file and
// names the cache from it; every navigation re-reads it, so a deploy
// retires the previous cache on the next page load.

const ASSET_NAMES = [
  "alcy_playground.js",
  "alcy_playground.wasm",
  "web-tree-sitter.js",
  "web-tree-sitter.wasm",
  "tree-sitter-alcy.wasm",
];

const CACHE_PREFIX = "alcy-assets-";
const VERSION_FILE = "asset-version.json";

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

function currentCacheName(): Promise<string> {
  if (cacheName === null) {
    cacheName = readCacheName();
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
  if (!ASSET_NAMES.some((name) => request.url.endsWith("/" + name))) {
    return;
  }
  event.respondWith(
    (async () => {
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
    })(),
  );
});
