// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's code blocks. The generator stamps each block's buttons and
// leaves them hidden, so a reader without JavaScript sees only the code;
// this module reveals them and wires Copy. The `alcy` blocks can also be
// run and edited, which the rest of this module covers.

import { t } from "./site.js";

const COPIED_MS = 1500;

interface CodeBlock {
  code: HTMLElement;
  actions: HTMLElement;
}

function findBlocks(): CodeBlock[] {
  const found: CodeBlock[] = [];
  for (const root of document.querySelectorAll<HTMLElement>(".code-block")) {
    const code = root.querySelector<HTMLElement>("pre code");
    const actions = root.querySelector<HTMLElement>(".code-actions");
    if (code !== null && actions !== null) {
      found.push({ code, actions });
    }
  }
  return found;
}

async function writeClipboard(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    // A denied clipboard or an insecure context: the old path still
    // copies, and a failure just leaves the button as it was.
    const scratch = document.createElement("textarea");
    scratch.value = text;
    scratch.setAttribute("readonly", "");
    scratch.style.position = "fixed";
    scratch.style.opacity = "0";
    document.body.append(scratch);
    scratch.select();
    let copied = false;
    try {
      copied = document.execCommand("copy");
    } catch {
      copied = false;
    }
    scratch.remove();
    return copied;
  }
}

function copy(block: CodeBlock, button: HTMLButtonElement): void {
  void writeClipboard(block.code.textContent ?? "").then((copied) => {
    if (!copied) {
      return;
    }
    button.textContent = t("guide.copied");
    window.setTimeout(() => {
      button.textContent = t("guide.copy");
    }, COPIED_MS);
  });
}

function wireCopy(block: CodeBlock): void {
  const button = block.actions.querySelector<HTMLButtonElement>('[data-action="copy"]');
  if (button === null) {
    return;
  }
  button.addEventListener("click", () => copy(block, button));
}

for (const block of findBlocks()) {
  block.actions.hidden = false;
  wireCopy(block);
}
