// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The entry point of the pages that share the site shell: the landing
// page and every guide page. The playground has its own entry and wires
// the same store to its own settings button.

import { elements } from "./elements.js";
import { initSearch } from "./search.js";
import { initThemeToggle, registerAssetWorker } from "./site.js";

initThemeToggle(elements.theme);
registerAssetWorker();
initSearch();

// The page outline reads open beside the article on a wide screen,
// where its sticky column has the room; a narrow one keeps it one tap
// above the reading, closed.
const outline = document.querySelector<HTMLDetailsElement>(".guide-outline");
if (outline !== null && window.matchMedia("(min-width: 761px)").matches) {
  outline.open = true;
}
