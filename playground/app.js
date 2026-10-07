// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The playground page's wiring: build the page's modules, connect their
// callbacks, and start the flows. Everything else lives in the feature
// modules next to this file.

import { elements } from "./elements.js";
import * as editor from "./editor.js";
import * as language from "./language.js";
import * as output from "./output.js";
import * as problems from "./problems.js";
import * as runtime from "./runtime.js";
import * as samples from "./samples.js";
import * as session from "./session.js";
import * as splitter from "./splitter.js";
import { state } from "./state.js";
import * as status from "./status.js";
import * as tabs from "./tabs.js";
import * as theme from "./theme.js";

// Everything that keeps its own rendered state is redrawn from the state
// it came from, so switching the language never leaves a mixed page.
function renderForLanguage() {
  status.renderStatus();
  status.renderCompilerState();
  output.renderRunMeta();
  status.setRunning(state.running);
  if (state.compilerErrorShown && state.compilerStatus === "error") {
    status.showCompilerError(state.compilerDetail);
  } else {
    problems.renderLast();
  }
}

function onInput() {
  runtime.scheduleCheck();
  session.scheduleCodeSave();
}

function init() {
  theme.initTheme();
  language.applyLanguage(language.storedLanguage());
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
  elements.check.addEventListener("click", () => void runtime.runCheck({ automatic: false }));
  elements.language.addEventListener("change", () => {
    const preference = elements.language.value;
    language.persistLanguage(preference);
    language.applyLanguage(preference);
    renderForLanguage();
  });

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
