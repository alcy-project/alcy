// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The example picker: the manifest the build assembled, the programs it
// names, and the restore path that prefers the last session over the
// default example.

import { FALLBACK_SOURCE, setText } from "./editor.js";
import { elements } from "./elements.js";
import { renderDiagnostics } from "./problems.js";
import { scheduleCheck } from "./runtime.js";
import { readSavedCode, saveCodeNow } from "./session.js";
import { setStatus } from "./status.js";

function applySource(text) {
  renderDiagnostics([]);
  setText(text);
  scheduleCheck();
}

export async function loadSample(file) {
  try {
    const response = await fetch(`samples/${file}`);
    applySource(await response.text());
    saveCodeNow();
  } catch (error) {
    setStatus("status.loadExampleFailed", { detail: error.message ?? String(error) });
  }
}

export async function loadSamples() {
  let manifest = null;
  try {
    manifest = await (await fetch("samples/index.json")).json();
  } catch {
    manifest = null;
  }
  if (manifest === null || !Array.isArray(manifest.samples) || manifest.samples.length === 0) {
    elements.source.value = FALLBACK_SOURCE;
    return;
  }
  elements.samples.replaceChildren();
  for (const sample of manifest.samples) {
    const option = document.createElement("option");
    option.value = sample.file;
    option.textContent = sample.title ?? sample.file;
    option.title = sample.description ?? "";
    elements.samples.append(option);
  }
  elements.samples.disabled = false;
  const first = manifest.default ?? manifest.samples[0].file;
  const initial = manifest.samples.some((sample) => sample.file === first)
    ? first
    : manifest.samples[0].file;
  const saved = readSavedCode();
  const restored =
    saved !== null &&
    saved.sampleFile !== null &&
    manifest.samples.some((sample) => sample.file === saved.sampleFile);
  elements.samples.value = restored ? saved.sampleFile : initial;
  if (saved !== null) {
    applySource(saved.source);
    setStatus("status.restored");
  } else {
    await loadSample(elements.samples.value);
  }
  elements.samples.addEventListener("change", () => loadSample(elements.samples.value));
}
