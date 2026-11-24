// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The entry point of the pages that share the site shell: the landing
// page and every guide page. The playground has its own entry and wires
// the same store to its own settings button.

import { elements } from "./elements.js";
import { initSearch } from "./search.js";
import { enableLinkPrefetch, initThemeToggle, registerAssetWorker } from "./site.js";

initThemeToggle(elements.theme);
registerAssetWorker();
initSearch();
enableLinkPrefetch();

// The wide-screen aside reads open beside the article; the narrow-screen
// drawer keeps its outline closed until the reader asks for it.
const outline = document.querySelector<HTMLDetailsElement>(
  ".guide-aside .guide-outline",
);
if (outline !== null && window.matchMedia("(min-width: 761px)").matches) {
  outline.open = true;
}

// The drawer's toggle is a native details summary. Following one of its
// links, Escape, or a click outside all close it again.
const drawer = document.querySelector<HTMLDetailsElement>(".guide-drawer");
if (drawer !== null) {
  drawer.addEventListener("click", (event) => {
    if (event.target instanceof Element && event.target.closest("a") !== null) {
      drawer.open = false;
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && drawer.open) {
      drawer.open = false;
    }
  });
  document.addEventListener("pointerdown", (event) => {
    if (drawer.open && event.target instanceof Node &&
        !drawer.contains(event.target)) {
      drawer.open = false;
    }
  });
}
