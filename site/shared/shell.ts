// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The entry point of the pages that share the site shell: the landing
// page and every guide page. The playground has its own entry and wires
// the same store to its own settings button.

import { elements } from "./elements.js";
import { initThemeToggle } from "./site.js";

initThemeToggle(elements.theme);
