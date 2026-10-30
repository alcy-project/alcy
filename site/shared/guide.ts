// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide page's wiring, split along what the reader presses. Run, the
// compiler's client, and the output are `guide-run.js`, which the page
// fetches because it is the common case. The editor, the colors, and the
// hand-off are `guide-edit.js`, fetched the first time a reader presses
// Edit: until then the published text and its Copy, Run, and Open answer
// for themselves.

import { findBlocks, type CodeBlock } from "./guide-copy.js";
import { wireRun } from "./guide-run.js";

// One promise per page: two Edits in the same tick share the fetch, and
// a load that fails leaves the buttons that were already wired alone.
let editorLoad: Promise<void> | null = null;
// The blocks whose Edit press is waiting for the module, in press order,
// so the press that opened the editor is the one that starts editing.
const pending: CodeBlock[] = [];

function loadEditor(): Promise<void> {
  if (editorLoad === null) {
    editorLoad = import("./guide-edit.js").then(
      (module) => {
        for (const block of pending) {
          module.wireEditor(block, block === pending[0]);
        }
        pending.length = 0;
      },
      () => {
        // Without the editor half the block keeps its published text and
        // its Run, which is the page it shipped with.
        editorLoad = null;
      },
    );
  }
  return editorLoad;
}

// The press that asks for the editor is answered by the editor, so the
// button waits for the half that will act on it rather than appearing to
// do nothing.
function onEditPress(block: CodeBlock): void {
  pending.push(block);
  void loadEditor();
}

for (const block of findBlocks()) {
  if (!block.alcy) {
    continue;
  }
  wireRun(block);
  const edit = block.actions.querySelector<HTMLButtonElement>('[data-action="edit"]');
  if (edit !== null) {
    edit.addEventListener("click", () => onEditPress(block), { once: true });
  }
}
