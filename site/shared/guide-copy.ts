// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide's code blocks. The generator stamps each block's buttons and
// leaves them hidden, so a reader without JavaScript sees only the code;
// this module reveals them and wires Copy. The `alcy` blocks' Run and
// Edit live in `guide.js`, which a page loads only when it holds a
// complete program, so a page of ordinary examples pays for neither the
// compiler's client nor the editor.

import { writeClipboard } from "./clipboard.js";
import { t } from "./site.js";

const COPIED_MS = 1500;

export interface CodeBlock {
  root: HTMLElement;
  pre: HTMLElement;
  code: HTMLElement;
  actions: HTMLElement;
  output: HTMLElement | null;
  alcy: boolean;
  // The code as it should be copied or run right now: the published text,
  // or the reader's working copy.
  source(): string;
}

export function findBlocks(): CodeBlock[] {
  const found: CodeBlock[] = [];
  for (const root of document.querySelectorAll<HTMLElement>(".code-block")) {
    const pre = root.querySelector<HTMLElement>("pre");
    const code = root.querySelector<HTMLElement>("pre code");
    const actions = root.querySelector<HTMLElement>(".code-actions");
    if (pre !== null && code !== null && actions !== null) {
      found.push({
        root,
        pre,
        code,
        actions,
        output: root.querySelector<HTMLElement>(".code-output"),
        alcy: root.dataset.language === "alcy",
        source: () => code.textContent ?? "",
      });
    }
  }
  return found;
}

export function copy(block: CodeBlock, button: HTMLButtonElement): void {
  void writeClipboard(block.source()).then((copied) => {
    if (!copied) {
      return;
    }
    button.textContent = t("guide.copied");
    window.setTimeout(() => {
      button.textContent = t("guide.copy");
    }, COPIED_MS);
  });
}

export function wireCopy(block: CodeBlock): void {
  const button = block.actions.querySelector<HTMLButtonElement>('[data-action="copy"]');
  if (button === null) {
    return;
  }
  button.addEventListener("click", () => copy(block, button));
}
