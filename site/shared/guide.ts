// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The guide page's wiring, split along what the reader presses. Run and
// Open are immediate: they need neither the compiler's client nor the
// editor. Copy, Run, and Open work from the published text until an Edit
// changes what `source()` returns. The editor and its colors live in
// `guide-edit.js`, fetched the first time a reader presses Edit.

import { handOffToPlayground } from "./handoff.js";
import { findBlocks, wireCopy, type CodeBlock } from "./guide-copy.js";
import { wireRun } from "./guide-run.js";

// One import per page: two Edits in the same tick share the fetch. The
// module is what is cached, not a wiring pass, so a press after the load
// still reaches the block that made it. A failed load is forgotten so the
// next press retries.
type EditorModule = typeof import("./guide-edit.js");
let editorPromise: Promise<EditorModule> | null = null;

function editorModule(): Promise<EditorModule> {
  if (editorPromise === null) {
    editorPromise = import("./guide-edit.js").catch((error: unknown) => {
      editorPromise = null;
      throw error;
    });
  }
  return editorPromise;
}

// The press that asks for the editor is answered by the editor, so the
// button waits for the half that will act on it rather than appearing to
// do nothing. `wireEditor` owns the once-only guard; a press that arrives
// while the import is in flight gets its own editor too.
function onEditPress(block: CodeBlock): void {
  void editorModule().then(
    (module) => module.wireEditor(block, true),
    () => undefined,
  );
}

// Open hands the block's current source to the reader's own tree's
// playground. The 404's block has no Open: its links already reach the
// playground, and its depth under the deployment root has no fixed
// "../" to land there.
function wireOpen(block: CodeBlock): void {
  const open = block.actions.querySelector<HTMLButtonElement>('[data-action="open"]');
  if (open === null) {
    return;
  }
  open.addEventListener("click", () => {
    handOffToPlayground(block.source());
    window.location.assign(new URL("../playground/", window.location.href).href);
  });
}

// Copy is every block's, so the row is revealed before the `alcy`-only
// wiring below narrows to the blocks that have Run and Edit.
for (const block of findBlocks()) {
  wireCopy(block);
  if (!block.alcy) {
    continue;
  }
  wireRun(block);
  wireOpen(block);
  const edit = block.actions.querySelector<HTMLButtonElement>('[data-action="edit"]');
  if (edit !== null) {
    edit.addEventListener("click", () => onEditPress(block));
  }
}
