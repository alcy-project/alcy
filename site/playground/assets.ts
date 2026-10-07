// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The deployed playground's own directory. A page may live in another
// language's tree (`/ja/playground/`) while the assets stay in one place,
// so they are resolved against this module's URL: whichever page loaded
// `app.js`, `compiler.worker.js`, the grammar, and the samples are its
// neighbours, not the page's.

export const assetBase = new URL(".", import.meta.url);
