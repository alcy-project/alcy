// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The playground page's wiring: build the page's modules, connect their
// callbacks, and start the flows. Everything else lives in the feature
// modules next to this file.

import { enableLinkPrefetch, initThemeToggle, registerAssetWorker } from "../shared/site.js";
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

// The kebab is a <details> whose actions sit in a sibling row, so the
// browser does not close it when one of those actions is used.
function initActionsMenu(): void {
  const menu = document.querySelector<HTMLDetailsElement>(".actions-menu");
  if (menu === null) {
    return;
  }
  const group = menu.closest(".run-group");
  const close = (): void => {
    menu.open = false;
  };
  document.addEventListener("pointerdown", (event) => {
    if (menu.open && group !== null && event.target instanceof Node &&
        !group.contains(event.target)) {
      close();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && menu.open) {
      close();
      menu.querySelector("summary")?.focus();
    }
  });
  for (const button of document.querySelectorAll(".toolbar-actions button")) {
    button.addEventListener("click", close);
  }
}

function init(): void {
  initThemeToggle(elements.theme);
  initActionsMenu();
  registerAssetWorker();
  initSearch();
  enableLinkPrefetch();
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
  elements.download.addEventListener("click", () => void runtime.runDownload());
  elements.format.addEventListener("click", () => void runtime.runFormat());
  elements.lint.addEventListener("click", () => void runtime.runLint());
  elements.openFile.addEventListener("click", () => elements.openFileInput.click());
  elements.openFileInput.addEventListener("change", () => {
    const file = elements.openFileInput.files?.[0];
    // Clearing first lets the same file be chosen again after a cancel.
    elements.openFileInput.value = "";
    if (file !== undefined) {
      void samples.openFile(file);
    }
  });
  runtime.warmOnIntent();

  // A reader who stays is likely to try something. Idle time is free
  // unless the connection says otherwise, which `warmCompiler` checks.
  if (typeof window.requestIdleCallback === "function") {
    window.requestIdleCallback(() => runtime.warmCompiler(), { timeout: 4000 });
  } else {
    window.setTimeout(() => runtime.warmCompiler(), 2000);
  }

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
