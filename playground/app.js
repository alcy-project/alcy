// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The playground page: an editor with tree-sitter highlighting on top,
// and the compiler wasm module behind the Check and Run buttons.
//
// Two workers do the work. `compiler.worker.js` owns the compiler module,
// which is expensive to load and instantiate, and is reused across
// requests. `runner.worker.js` instantiates one compiled program and is
// thrown away after each run, so terminating a program that never
// returns costs nothing but that worker.

import { resolveLanguage, translate } from "./i18n.js";
import { byteToUtf16Map, utf16IndexAtByte } from "./textutil.js";

const CHECK_DEBOUNCE_MS = 500;
const RUN_TIMEOUT_MS = 5000;
const THEME_KEY = "alcy-playground-theme";
const LANGUAGE_KEY = "alcy-playground-language";
const CODE_KEY = "alcy-playground-code";
const CODE_SAVE_DEBOUNCE_MS = 300;
const SPLIT_COLUMN_KEY = "alcy-playground-side-fraction";
const SPLIT_ROW_KEY = "alcy-playground-row-fraction";
const DEFAULT_COLUMN_FRACTION = 0.38;
const DEFAULT_ROW_FRACTION = 0.45;
const MIN_COLUMN_PX = 220;
const MIN_ROW_PX = 150;
// The layout stacks below this width; keep it in step with the breakpoint
// in style.css.
const NARROW_QUERY = "(max-width: 760px)";

const FALLBACK_SOURCE = `fn main() {\n  println("Hello, alcy!")\n}\n`;

const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");
const narrowQuery = window.matchMedia(NARROW_QUERY);

const elements = {
  source: document.getElementById("source"),
  // The mirror's scroll container is the <pre>; the <code> inside it is
  // what receives the highlighted HTML.
  highlight: document.getElementById("highlight"),
  highlightCode: document.getElementById("highlight-code"),
  gutter: document.getElementById("gutter"),
  layout: document.querySelector(".layout"),
  splitter: document.getElementById("splitter"),
  samples: document.getElementById("samples"),
  theme: document.getElementById("theme"),
  language: document.getElementById("language"),
  check: document.getElementById("check"),
  run: document.getElementById("run"),
  compilerState: document.getElementById("compiler-state"),
  compilerStateText: document.getElementById("compiler-state-text"),
  status: document.getElementById("status"),
  tabProblems: document.getElementById("tab-problems"),
  tabOutput: document.getElementById("tab-output"),
  panelProblems: document.getElementById("panel-problems"),
  panelOutput: document.getElementById("panel-output"),
  problems: document.getElementById("problems"),
  problemsEmpty: document.getElementById("problems-empty"),
  problemsCount: document.getElementById("problems-count"),
  runMeta: document.getElementById("run-meta"),
  stdout: document.getElementById("stdout"),
  stderr: document.getElementById("stderr"),
};

const state = {
  highlighter: null,
  compiler: null,
  compilerStatus: "idle",
  compilerDetail: "",
  requests: new Map(),
  requestSeq: 0,
  checkSeq: 0,
  highlightFrame: null,
  highlightedText: null,
  gutterLines: 0,
  sideFraction: DEFAULT_COLUMN_FRACTION,
  codeSaveTimer: null,
  checkTimer: null,
  running: false,
  runner: null,
  runnerTimer: null,
  language: "en",
  status: { key: "status.ready" },
  runMeta: null,
  lastDiagnostics: [],
  compilerErrorShown: false,
};

function t(key, params) {
  return translate(state.language, key, params);
}

// Chrome and tabs -----------------------------------------------------------

// Status messages are kept as a catalog key so a language switch
// re-renders the line.
function renderStatus() {
  elements.status.textContent = t(state.status.key, state.status.params);
}

function setStatus(key, params) {
  state.status = { key, params };
  renderStatus();
}

function showTab(name) {
  const problems = name === "problems";
  elements.tabProblems.classList.toggle("active", problems);
  elements.tabOutput.classList.toggle("active", !problems);
  elements.tabProblems.setAttribute("aria-selected", String(problems));
  elements.tabOutput.setAttribute("aria-selected", String(!problems));
  elements.panelProblems.classList.toggle("hidden", !problems);
  elements.panelOutput.classList.toggle("hidden", problems);
}

function setCompilerState(status, detail = "") {
  state.compilerStatus = status;
  state.compilerDetail = detail;
  renderCompilerState();
  if (status === "ready") {
    scheduleCheck();
  }
  if (status === "error") {
    showCompilerError(detail);
  }
}

function renderCompilerState() {
  elements.compilerState.dataset.state = state.compilerStatus;
  elements.compilerStateText.textContent = t(`compiler.${state.compilerStatus}`);
  elements.compilerState.title = state.compilerDetail;
}

function showCompilerError(detail) {
  state.lastDiagnostics = [
    {
      severity: "error",
      code: null,
      message: t("compiler.missing", { detail: detail ? `: ${detail}` : "" }),
      span: null,
      labels: [],
    },
  ];
  renderDiagnostics(state.lastDiagnostics);
  // The message is this module's own text, so a language switch has to
  // re-render it; a diagnostic from the compiler is kept verbatim.
  state.compilerErrorShown = true;
  showTab("problems");
  setStatus("status.compilerUnavailable");
}

function setRunning(running) {
  state.running = running;
  elements.run.disabled = running;
  elements.check.disabled = running;
  elements.run.classList.toggle("busy", running);
  elements.run.setAttribute("aria-label", t(running ? "button.running" : "button.run"));
}

// Theme ---------------------------------------------------------------------

function storedTheme() {
  try {
    return localStorage.getItem(THEME_KEY) ?? "auto";
  } catch {
    return "auto";
  }
}

function applyTheme(mode) {
  const dark = mode === "dark" || (mode === "auto" && darkQuery.matches);
  document.documentElement.dataset.theme = dark ? "dark" : "light";
  elements.theme.value = mode;
}

function initTheme() {
  applyTheme(storedTheme());
  elements.theme.addEventListener("change", () => {
    const mode = elements.theme.value;
    try {
      localStorage.setItem(THEME_KEY, mode);
    } catch {
      // A denied store costs the preference, not the theme.
    }
    applyTheme(mode);
  });
  darkQuery.addEventListener("change", () => {
    if (storedTheme() === "auto") {
      applyTheme("auto");
    }
  });
}

// Language ------------------------------------------------------------------

function storedLanguage() {
  try {
    return localStorage.getItem(LANGUAGE_KEY) ?? "auto";
  } catch {
    return "auto";
  }
}

function applyI18n() {
  document.documentElement.lang = state.language;
  for (const node of document.querySelectorAll("[data-i18n]")) {
    node.textContent = t(node.dataset.i18n);
  }
  for (const node of document.querySelectorAll("[data-i18n-aria]")) {
    node.setAttribute("aria-label", t(node.dataset.i18nAria));
  }
  for (const node of document.querySelectorAll("[data-i18n-title]")) {
    node.title = t(node.dataset.i18nTitle);
  }
}

// Every piece of text that is not a static attribute is re-rendered from
// the translation of the state it came from, so switching the language
// never leaves a mixed page behind.
function applyLanguage(preference) {
  state.language = resolveLanguage(preference);
  elements.language.value = preference === "auto" ? "auto" : state.language;
  applyI18n();
  renderStatus();
  renderCompilerState();
  renderRunMeta();
  setRunning(state.running);
  if (state.compilerErrorShown && state.compilerStatus === "error") {
    showCompilerError(state.compilerDetail);
  } else {
    renderDiagnostics(state.lastDiagnostics);
  }
}

function initLanguage() {
  applyLanguage(storedLanguage());
  elements.language.addEventListener("change", () => {
    const preference = elements.language.value;
    try {
      localStorage.setItem(LANGUAGE_KEY, preference);
    } catch {
      // A denied store costs the preference, not the language.
    }
    applyLanguage(preference);
  });
}

// Splitter ------------------------------------------------------------------
//
// One separator serves both layouts: side by side it is vertical and the
// fraction is the right pane's share of the width; stacked it is
// horizontal and the fraction is the bottom pane's share of the height.
// The two remember their own fraction, because a comfortable split in one
// layout is rarely one in the other.

function currentSplitKey() {
  return narrowQuery.matches ? SPLIT_ROW_KEY : SPLIT_COLUMN_KEY;
}

function defaultSideFraction() {
  return narrowQuery.matches ? DEFAULT_ROW_FRACTION : DEFAULT_COLUMN_FRACTION;
}

function storedSideFraction() {
  try {
    const stored = Number.parseFloat(localStorage.getItem(currentSplitKey()));
    return Number.isFinite(stored) && stored > 0 && stored < 1
      ? stored
      : defaultSideFraction();
  } catch {
    return defaultSideFraction();
  }
}

function persistSideFraction(fraction) {
  try {
    localStorage.setItem(currentSplitKey(), String(fraction));
  } catch {
    // A denied store costs the preference, not the split.
  }
}

// Clamping happens here so the grid can only be given a share both panes
// survive. The clamped value is what the state keeps, so a keypress at an
// edge does not have to walk back through unclamped values.
function applySideFraction(requested) {
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

function initSplitter() {
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

  const stopDragging = (event) => {
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

// Editor --------------------------------------------------------------------

function updateGutter() {
  const lines = elements.source.value.split("\n").length;
  if (lines === state.gutterLines) {
    return;
  }
  state.gutterLines = lines;
  elements.gutter.textContent = Array.from({ length: lines }, (_, i) => i + 1).join("\n");
  elements.gutter.style.minWidth = `${String(lines).length + 1}ch`;
}

function syncScroll() {
  // iOS overscroll reports offsets past either end of the range while
  // rubber-banding. The mirror clamps those silently when they are
  // assigned to its scrollTop, but the transform would move the gutter
  // alone; clamp here so both sides see the same value. The metrics are
  // read before anything is written, which keeps this a clean-layout
  // query on a scroll frame.
  const source = elements.source;
  const maxTop = Math.max(0, source.scrollHeight - source.clientHeight);
  const maxLeft = Math.max(0, source.scrollWidth - source.clientWidth);
  const top = Math.min(Math.max(source.scrollTop, 0), maxTop);
  const left = Math.min(Math.max(source.scrollLeft, 0), maxLeft);
  elements.highlight.scrollTop = top;
  elements.highlight.scrollLeft = left;
  elements.gutter.style.transform = `translateY(${-top}px)`;
}

// The mirror hides its scrollbars while the textarea shows them, so it
// has to pad by the space they take or the two clamp at different scroll
// maxima. The ResizeObserver on the textarea calls this when a scrollbar
// appears or disappears, which is the only time the numbers change.
function syncMetrics() {
  const scrollbarWidth = elements.source.offsetWidth - elements.source.clientWidth;
  const scrollbarHeight = elements.source.offsetHeight - elements.source.clientHeight;
  elements.highlight.style.setProperty(
    "--editor-scrollbar-width",
    `${Math.max(0, scrollbarWidth)}px`,
  );
  elements.highlight.style.setProperty(
    "--editor-scrollbar-height",
    `${Math.max(0, scrollbarHeight)}px`,
  );
}

function insertAtCursor(text) {
  const start = elements.source.selectionStart;
  const end = elements.source.selectionEnd;
  const value = elements.source.value;
  elements.source.value = value.slice(0, start) + text + value.slice(end);
  elements.source.selectionStart = elements.source.selectionEnd = start + text.length;
}

// The mirror is plain text the moment the textarea changes, so the glyphs
// under the caret are never stale; the colors arrive when the parse that
// produces them is done. A parse that cannot run (or fails) leaves the
// plain text in place.
function updateMirrorPlain() {
  state.highlightedText = null;
  elements.highlightCode.textContent = elements.source.value;
  syncScroll();
}

function scheduleHighlight() {
  updateMirrorPlain();
  if (state.highlightFrame !== null) {
    return;
  }
  state.highlightFrame = window.requestAnimationFrame(() => {
    state.highlightFrame = null;
    renderHighlight();
  });
}

function renderHighlight() {
  if (state.highlighter === null) {
    return;
  }
  const text = elements.source.value;
  if (text === state.highlightedText) {
    return;
  }
  try {
    elements.highlightCode.innerHTML = state.highlighter.highlight(text);
    state.highlightedText = text;
    syncScroll();
  } catch (error) {
    state.highlighter = null;
    state.highlightedText = null;
    setStatus("status.highlightingDisabled", { detail: error.message ?? String(error) });
    updateMirrorPlain();
  }
}

// Colors are an enhancement, so the runtime, the grammar, and the query
// are fetched together once the page is idle: first paint and typing do
// not wait for a wasm download. Plain text is already on screen.
function whenIdle(callback) {
  if (typeof window.requestIdleCallback === "function") {
    window.requestIdleCallback(callback, { timeout: 1500 });
  } else {
    window.setTimeout(callback, 200);
  }
}

async function initHighlighter() {
  const runtimeUrl = new URL("vendor/web-tree-sitter.wasm", document.baseURI).href;
  const grammarUrl = new URL("grammar/tree-sitter-alcy.wasm", document.baseURI).href;
  try {
    const [module, queryResponse, runtimeBytes, grammarBytes] = await Promise.all([
      import("./highlight.js"),
      fetch("grammar/highlights.scm"),
      fetch(runtimeUrl).then((response) => response.arrayBuffer()),
      fetch(grammarUrl).then((response) => response.arrayBuffer()),
    ]);
    state.highlighter = await module.createHighlighter({
      runtimeWasm: runtimeBytes,
      languageWasm: new Uint8Array(grammarBytes),
      querySource: await queryResponse.text(),
    });
    renderHighlight();
  } catch (error) {
    setStatus("status.highlightingUnavailable", { detail: error.message ?? String(error) });
  }
}

// Diagnostics ---------------------------------------------------------------

function lineColumnAt(text, index) {
  let line = 1;
  let lineStart = 0;
  for (let i = 0; i < index; i++) {
    if (text.charCodeAt(i) === 10) {
      line++;
      lineStart = i + 1;
    }
  }
  return { line, column: index - lineStart + 1 };
}

function selectionRangeFor(span) {
  const text = elements.source.value;
  const table = byteToUtf16Map(text);
  const start = utf16IndexAtByte(table, span.offset);
  const end = utf16IndexAtByte(table, span.offset + span.length);
  return { start, end };
}

function selectSpan(span) {
  const { start, end } = selectionRangeFor(span);
  const text = elements.source.value;
  const { line } = lineColumnAt(text, start);
  const lineHeight = Number.parseFloat(getComputedStyle(elements.source).lineHeight) || 20;
  const firstVisible = elements.source.scrollTop / lineHeight;
  const visibleLines = elements.source.clientHeight / lineHeight;
  elements.source.focus();
  elements.source.setSelectionRange(start, end);
  if (line < firstVisible + 2 || line > firstVisible + visibleLines - 2) {
    elements.source.scrollTop = Math.max(0, (line - 3) * lineHeight);
  }
  syncScroll();
}

function codeLabel(code) {
  if (code === null || code === undefined) {
    return "";
  }
  return `${code.stage} ${code.local_id}`;
}

// The badge says how loud the loudest diagnostic is rather than always
// reading as an error.
function worstSeverity(diagnostics) {
  let worst = "note";
  for (const diagnostic of diagnostics) {
    const severity = diagnostic.severity ?? "error";
    if (severity === "error") {
      return "error";
    }
    if (severity === "warning") {
      worst = "warning";
    }
  }
  return worst;
}

function renderDiagnostics(diagnostics) {
  state.lastDiagnostics = diagnostics;
  state.compilerErrorShown = false;
  elements.problems.replaceChildren();
  elements.problemsEmpty.hidden = diagnostics.length > 0;
  elements.problemsCount.hidden = diagnostics.length === 0;
  elements.problemsCount.textContent = String(diagnostics.length);
  elements.problemsCount.dataset.severity = worstSeverity(diagnostics);

  const text = elements.source.value;
  const table =
    diagnostics.some((diagnostic) => diagnostic.span !== null)
      ? byteToUtf16Map(text)
      : null;

  for (const diagnostic of diagnostics) {
    const item = document.createElement("li");
    const severityName = diagnostic.severity ?? "error";
    item.className =
      severityName === "warning" || severityName === "note" ? severityName : "error";

    const head = document.createElement("div");
    head.className = "problem-head";

    const severity = document.createElement("span");
    severity.className = `severity ${severityName}`;
    severity.textContent = t(`severity.${severityName}`);
    head.append(severity);

    if (diagnostic.span) {
      const start = utf16IndexAtByte(table, diagnostic.span.offset);
      const { line, column } = lineColumnAt(text, start);
      const location = document.createElement("span");
      location.className = "problem-location";
      location.textContent = `${diagnostic.span.file ?? "source"}:${line}:${column}`;
      head.append(location);
    }

    const label = codeLabel(diagnostic.code);
    if (label !== "") {
      const code = document.createElement("span");
      code.className = "problem-code";
      code.textContent = label;
      head.append(code);
    }

    const message = document.createElement("div");
    message.className = "problem-message";
    message.textContent = diagnostic.message ?? "";

    item.append(head, message);
    if (diagnostic.span) {
      item.addEventListener("click", () => selectSpan(diagnostic.span));
    }
    elements.problems.append(item);
  }
}

// Compiler worker -----------------------------------------------------------

function ensureCompilerWorker() {
  if (state.compiler !== null) {
    return state.compiler;
  }
  const worker = new Worker("compiler.worker.js");
  worker.addEventListener("message", (event) => {
    const message = event.data ?? {};
    if (message.type === "status") {
      setCompilerState(message.status, message.detail);
      return;
    }
    if (message.type === "response") {
      const pending = state.requests.get(message.id);
      if (pending === undefined) {
        return;
      }
      state.requests.delete(message.id);
      pending.resolve(message);
    }
  });
  worker.addEventListener("error", (event) => {
    failAllRequests(event.message ?? "the compiler worker stopped");
    worker.terminate();
    state.compiler = null;
    setCompilerState("error", event.message ?? "");
  });
  worker.addEventListener("messageerror", () => {
    failAllRequests("the compiler worker sent an unreadable message");
  });
  state.compiler = worker;
  return worker;
}

function failAllRequests(message) {
  for (const pending of state.requests.values()) {
    pending.reject(new Error(message));
  }
  state.requests.clear();
}

function callCompiler(op, source) {
  const worker = ensureCompilerWorker();
  const id = ++state.requestSeq;
  return new Promise((resolve, reject) => {
    state.requests.set(id, { resolve, reject });
    worker.postMessage({ type: "request", id, op, source });
  });
}

function scheduleCheck() {
  if (state.compilerStatus !== "ready") {
    return;
  }
  window.clearTimeout(state.checkTimer);
  state.checkTimer = window.setTimeout(() => {
    runCheck({ automatic: true });
  }, CHECK_DEBOUNCE_MS);
}

function normalizeSource() {
  return elements.source.value.replaceAll("\r\n", "\n");
}

async function runCheck({ automatic = false } = {}) {
  if (state.running) {
    return;
  }
  const sequence = ++state.checkSeq;
  if (!automatic) {
    setStatus("status.checking");
  }
  try {
    const started = performance.now();
    const response = await callCompiler("check", normalizeSource());
    if (sequence !== state.checkSeq) {
      return;
    }
    if (response.error) {
      // The load failure was already reported through the compiler state;
      // this response's empty diagnostics must not clear that report.
      return;
    }
    renderDiagnostics(response.diagnostics ?? []);
    if (!automatic) {
      showTab("problems");
      const ms = (performance.now() - started).toFixed(1);
      if (response.ok) {
        setStatus("status.checkOk", {
          functions: response.stats?.functionCount ?? 0,
          ms,
        });
      } else {
        setStatus("status.checkFailed", {
          count: response.diagnostics?.length ?? 0,
        });
      }
    }
  } catch (error) {
    if (sequence === state.checkSeq) {
      setCompilerState("error", error.message ?? String(error));
    }
  }
}

// Run -----------------------------------------------------------------------

// The meta line is a translated main sentence plus an optional suffix, so
// a language switch re-renders a finished run the same way.
function renderRunMeta() {
  if (state.runMeta === null) {
    elements.runMeta.textContent = "";
    return;
  }
  const { main, suffix } = state.runMeta;
  let text = main ? t(main.key, main.params) : "";
  if (suffix) {
    text += (text === "" ? "" : " · ") + t(suffix.key, suffix.params);
  }
  elements.runMeta.textContent = text;
}

function setRunMeta(main) {
  state.runMeta = { main, suffix: null };
  renderRunMeta();
}

function setRunMetaSuffix(suffix) {
  if (state.runMeta === null) {
    return;
  }
  state.runMeta.suffix = suffix;
  renderRunMeta();
}

async function run() {
  if (state.running) {
    return;
  }
  setRunning(true);
  showTab("output");
  setRunMeta({ key: "meta.compiling" });
  elements.stdout.textContent = "";
  elements.stderr.textContent = "";
  setStatus("status.compiling");

  try {
    const started = performance.now();
    const response = await callCompiler("compile", normalizeSource());
    if (response.error) {
      setRunMeta({ key: "meta.compileFailed" });
      setStatus("status.compilerUnavailable");
      return;
    }
    renderDiagnostics(response.diagnostics ?? []);
    if (!response.ok || response.wasm === null) {
      showTab("problems");
      setRunMeta({ key: "meta.compileFailed" });
      setStatus("status.compileFailed");
      return;
    }
    const ms = (performance.now() - started).toFixed(1);
    const bytes = response.wasm.byteLength;
    setRunMeta({
      key: "meta.compiled",
      params: { ms, bytes, functions: response.stats?.functionCount ?? 0 },
    });
    setStatus("status.running");
    await execute(response.wasm);
  } catch (error) {
    showTab("problems");
    renderDiagnostics([
      {
        severity: "error",
        code: null,
        message: error.message ?? String(error),
        span: null,
        labels: [],
      },
    ]);
    setStatus("status.compilerError");
  } finally {
    setRunning(false);
  }
}

function execute(wasmBuffer) {
  return new Promise((resolve) => {
    state.runner?.terminate();
    const runner = new Worker("runner.worker.js");
    state.runner = runner;
    state.runnerTimer = window.setTimeout(() => {
      runner.terminate();
      state.runner = null;
      elements.stderr.textContent = t("program.timedOut", {
        seconds: RUN_TIMEOUT_MS / 1000,
      });
      setRunMetaSuffix({ key: "meta.timedOut" });
      setStatus("status.timedOut");
      resolve();
    }, RUN_TIMEOUT_MS);

    runner.addEventListener("message", (event) => {
      const message = event.data ?? {};
      window.clearTimeout(state.runnerTimer);
      runner.terminate();
      state.runner = null;
      if (message.type === "result") {
        elements.stdout.textContent = message.stdout;
        elements.stderr.textContent = message.stderr;
        setRunMetaSuffix({ key: "meta.exitCode", params: { code: message.exitCode } });
        if (message.exitCode === 0) {
          setStatus("status.runFinished");
        } else {
          setStatus("status.exited", { code: message.exitCode });
        }
      } else {
        elements.stderr.textContent = message.message ?? "the program could not be instantiated";
        setRunMetaSuffix({ key: "meta.notRunnable" });
        setStatus("status.runFailed");
      }
      resolve();
    });

    runner.addEventListener("error", (event) => {
      window.clearTimeout(state.runnerTimer);
      elements.stderr.textContent = event.message ?? "the run worker stopped";
      setStatus("status.runFailed");
      resolve();
    });

    runner.postMessage({ type: "run", wasm: wasmBuffer }, [wasmBuffer]);
  });
}

// Session -------------------------------------------------------------------
//
// The editor's buffer and the sample it came from live in localStorage, so
// a reload returns to the code that was being worked on rather than to the
// default example. Writes are debounced; `pagehide` flushes the last one.

function readSavedCode() {
  try {
    const raw = localStorage.getItem(CODE_KEY);
    if (raw === null) {
      return null;
    }
    const saved = JSON.parse(raw);
    if (typeof saved?.source !== "string") {
      return null;
    }
    return {
      source: saved.source,
      sampleFile: typeof saved.sampleFile === "string" ? saved.sampleFile : null,
    };
  } catch {
    return null;
  }
}

function saveCodeNow() {
  window.clearTimeout(state.codeSaveTimer);
  state.codeSaveTimer = null;
  try {
    localStorage.setItem(
      CODE_KEY,
      JSON.stringify({
        version: 1,
        source: elements.source.value,
        sampleFile: elements.samples.value || null,
      }),
    );
  } catch {
    // A full or denied store costs the snapshot, not the session.
  }
}

function scheduleCodeSave() {
  window.clearTimeout(state.codeSaveTimer);
  state.codeSaveTimer = window.setTimeout(saveCodeNow, CODE_SAVE_DEBOUNCE_MS);
}

// Samples -------------------------------------------------------------------

function applySource(text) {
  elements.source.value = text;
  renderDiagnostics([]);
  scheduleHighlight();
  updateGutter();
  syncMetrics();
  elements.source.scrollTop = 0;
  syncScroll();
  scheduleCheck();
}

async function loadSamples() {
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

async function loadSample(file) {
  try {
    const response = await fetch(`samples/${file}`);
    applySource(await response.text());
    saveCodeNow();
  } catch (error) {
    setStatus("status.loadExampleFailed", { detail: error.message ?? String(error) });
  }
}

// Wiring --------------------------------------------------------------------

function init() {
  elements.source.value = FALLBACK_SOURCE;
  updateGutter();
  syncMetrics();
  updateMirrorPlain();

  elements.source.addEventListener("input", () => {
    scheduleHighlight();
    updateGutter();
    scheduleCheck();
    scheduleCodeSave();
  });
  elements.source.addEventListener("scroll", syncScroll);
  // The wheel over the line numbers belongs to the editor, not to the
  // gutter, which is not a scroll container.
  elements.gutter.addEventListener(
    "wheel",
    (event) => {
      elements.source.scrollTop += event.deltaY;
      elements.source.scrollLeft += event.deltaX;
      event.preventDefault();
    },
    { passive: false },
  );
  elements.source.addEventListener("keydown", (event) => {
    if (event.key === "Tab") {
      event.preventDefault();
      insertAtCursor("  ");
      scheduleHighlight();
      updateGutter();
      scheduleCheck();
      scheduleCodeSave();
    } else if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
      event.preventDefault();
      run();
    }
  });

  elements.run.addEventListener("click", run);
  elements.check.addEventListener("click", () => runCheck({ automatic: false }));
  elements.tabProblems.addEventListener("click", () => showTab("problems"));
  elements.tabOutput.addEventListener("click", () => showTab("output"));

  initTheme();
  initLanguage();
  initSplitter();
  new ResizeObserver(syncMetrics).observe(elements.source);
  // A reload or a tab switch right after a keystroke must not lose it:
  // both events flush the pending debounced save.
  window.addEventListener("pagehide", saveCodeNow);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
      saveCodeNow();
    }
  });
  whenIdle(() => void initHighlighter());
  void loadSamples();
}

init();
