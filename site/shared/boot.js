// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Resolves the color theme before first paint, from the stored choice or
// the system preference, so no page flashes the wrong one. This is a plain
// script for the head; `site.ts` owns the toggle afterwards, and its keys
// and resolution must stay in step with these.

(() => {
  let stored = "auto";
  try {
    stored =
      localStorage.getItem("alcy-site-theme") ??
      localStorage.getItem("alcy-playground-theme") ??
      "auto";
  } catch {
    // Storage can be denied; the system preference is the fallback.
  }
  const dark =
    stored === "dark" ||
    (stored === "auto" &&
      window.matchMedia("(prefers-color-scheme: dark)").matches);
  document.documentElement.dataset.theme = dark ? "dark" : "light";
})();
