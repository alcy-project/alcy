// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The entry point of the pages that share the site shell: the landing
// page and every guide page. The playground has its own entry and wires
// the same stores to its own elements.

import { initLanguage, initTheme } from "./site.js";
import { elements } from "./elements.js";

// The picks are enough while `language` is the only field of the store
// the shell needs. The i18n pass runs on the document.
initTheme(elements.theme);
initLanguage(elements.language);
