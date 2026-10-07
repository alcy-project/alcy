// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// One separator serves both layouts: side by side it is vertical and the
// fraction is the right pane's share of the width; stacked it is
// horizontal and the fraction is the bottom pane's share of the height.
// The two remember their own fraction, because a comfortable split in one
// layout is rarely one in the other.
//
// The breakpoint here must stay in step with the `max-width: 760px` query
// in style.css.

import { elements } from "./elements.js";
import { state } from "./state.js";

const SPLIT_COLUMN_KEY = "alcy-playground-side-fraction";
const SPLIT_ROW_KEY = "alcy-playground-row-fraction";
const DEFAULT_COLUMN_FRACTION = 0.38;
const DEFAULT_ROW_FRACTION = 0.45;
const MIN_COLUMN_PX = 220;
const MIN_ROW_PX = 150;
const NARROW_QUERY = "(max-width: 760px)";

const narrowQuery = window.matchMedia(NARROW_QUERY);

function currentSplitKey(): string {
  return narrowQuery.matches ? SPLIT_ROW_KEY : SPLIT_COLUMN_KEY;
}

function defaultSideFraction(): number {
  return narrowQuery.matches ? DEFAULT_ROW_FRACTION : DEFAULT_COLUMN_FRACTION;
}

function storedSideFraction(): number {
  try {
    const stored = Number.parseFloat(localStorage.getItem(currentSplitKey()) ?? "");
    return Number.isFinite(stored) && stored > 0 && stored < 1
      ? stored
      : defaultSideFraction();
  } catch {
    return defaultSideFraction();
  }
}

function persistSideFraction(fraction: number): void {
  try {
    localStorage.setItem(currentSplitKey(), String(fraction));
  } catch {
    // A denied store costs the preference, not the split.
  }
}

// Clamping happens here so the grid can only be given a share both panes
// survive. The clamped value is what the state keeps, so a keypress at an
// edge does not have to walk back through unclamped values.
function applySideFraction(requested: number): number {
  const stacked = narrowQuery.matches;
  const extent =
    (stacked ? elements.layout.clientHeight : elements.layout.clientWidth) || 1;
  const minPx = stacked ? MIN_ROW_PX : MIN_COLUMN_PX;
  const min = Math.min(minPx / extent, 0.45);
  const clamped = Math.min(Math.max(requested, min), 1 - min);
  state.sideFraction = clamped;
  elements.layout.style.setProperty(
    "--side-fraction",
    `${(clamped * 100).toFixed(2)}%`,
  );
  elements.splitter.setAttribute(
    "aria-orientation",
    stacked ? "horizontal" : "vertical",
  );
  elements.splitter.setAttribute("aria-valuenow", String(Math.round(clamped * 100)));
  elements.splitter.setAttribute("aria-valuemin", String(Math.round(min * 100)));
  elements.splitter.setAttribute("aria-valuemax", String(Math.round((1 - min) * 100)));
  return clamped;
}

export function initSplitter(): void {
  applySideFraction(storedSideFraction());
  const splitter = elements.splitter;

  splitter.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) {
      return;
    }
    splitter.setPointerCapture(event.pointerId);
    splitter.dataset.dragging = "true";
    document.body.classList.add("resizing");
    event.preventDefault();
  });

  splitter.addEventListener("pointermove", (event) => {
    if (splitter.dataset.dragging !== "true") {
      return;
    }
    const rect = elements.layout.getBoundingClientRect();
    const fraction = narrowQuery.matches
      ? (rect.bottom - event.clientY) / rect.height
      : (rect.right - event.clientX) / rect.width;
    persistSideFraction(applySideFraction(fraction));
  });

  const stopDragging = (event: PointerEvent): void => {
    if (splitter.dataset.dragging !== "true") {
      return;
    }
    splitter.dataset.dragging = "false";
    document.body.classList.remove("resizing");
    if (splitter.hasPointerCapture(event.pointerId)) {
      splitter.releasePointerCapture(event.pointerId);
    }
  };
  splitter.addEventListener("pointerup", stopDragging);
  splitter.addEventListener("pointercancel", stopDragging);
  splitter.addEventListener("lostpointercapture", stopDragging);

  // The arrow that grows the pane after the separator: Left in the side
  // by side layout, Up in the stacked one.
  splitter.addEventListener("keydown", (event) => {
    const grow = narrowQuery.matches ? "ArrowUp" : "ArrowLeft";
    const shrink = narrowQuery.matches ? "ArrowDown" : "ArrowRight";
    const step = 0.02;
    if (event.key === grow) {
      persistSideFraction(applySideFraction(state.sideFraction + step));
    } else if (event.key === shrink) {
      persistSideFraction(applySideFraction(state.sideFraction - step));
    } else {
      return;
    }
    event.preventDefault();
  });

  splitter.addEventListener("dblclick", () => {
    persistSideFraction(applySideFraction(defaultSideFraction()));
  });

  narrowQuery.addEventListener("change", () => {
    applySideFraction(storedSideFraction());
  });

  new ResizeObserver(() => applySideFraction(state.sideFraction)).observe(
    elements.layout,
  );
}
