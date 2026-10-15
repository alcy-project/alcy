// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The playground page's wiring: build the page's modules, connect their
// callbacks, and start the flows. Everything else lives in the feature
// modules next to this file.

import { initThemeToggle, registerAssetWorker } from "../shared/site.js";
import { initSearch } from "../shared/search.js";
import { writeClipboard } from "../shared/clipboard.js";

import { elements } from "./elements.js";
import * as editor from "./editor.js";
import * as output from "./output.js";
import * as problems from "./problems.js";
import * as runtime from "./runtime.js";
import * as samples from "./samples.js";
import * as session from "./session.js";
import * as splitter from "./splitter.js";
import { state } from "./state.js";
import * as status from "./status.js";
import * as tabs from "./tabs.js";

const LINK_COPIED_MS = 1500;

function copyLink(): void {
  void writeClipboard(session.shareLink(elements.source.value)).then((copied) => {
    if (!copied) {
      return;
    }
    elements.copyLink.dataset.copied = "true";
    window.setTimeout(() => {
      delete elements.copyLink.dataset.copied;
    }, LINK_COPIED_MS);
  });
}

function onInput(): void {
  runtime.scheduleCheck();
  session.scheduleCodeSave();
}

function init(): void {
  initThemeToggle(elements.theme);
  registerAssetWorker();
  initSearch();
  splitter.initSplitter();
  tabs.initTabs();
  status.renderStatus();
  status.renderCompilerState();
  status.setRunning(state.running);

  editor.initEditor({
    onInput,
    onRun: () => void runtime.run(),
    onHighlightError: status.setStatus,
  });

  elements.run.addEventListener("click", () => void runtime.run());
  elements.check.addEventListener("click", () => {
    void runtime.runCheck({ automatic: false });
  });
  elements.copyLink.addEventListener("click", copyLink);

  // A reload or a tab switch right after a keystroke must not lose it:
  // both events flush the pending debounced save.
  window.addEventListener("pagehide", session.saveCodeNow);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
      session.saveCodeNow();
    }
  });

  void samples.loadSamples();
}

init();
