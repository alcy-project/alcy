// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The site's own strings: the playground's chrome, the shell that every
// page shares, and the guide's navigation labels. Compiler diagnostics
// arrive from the compiler's catalogs and keep whatever language the
// compiler was built for; these cover everything around them.
//
// The site has one page tree per language: the generator stamps the
// static labels of each page from this catalog at build time (`t:` at
// the template level), and the playground's dynamic strings go through
// `t()` at runtime. Adding a language means one catalog here, one entry
// in `LANGUAGES`, and one tree for the generator to emit.

const en = {
  "site.guide": "Guide",
  "site.playground": "Playground",
  "site.previous": "Previous",
  "site.next": "Next",
  "label.settings": "Settings",
  "meta.guide": "the alcy guide",
  "noscript.playground": "The playground needs JavaScript to run.",
  "meta.playground":
    "A browser wasm playground for the alcy programming language: edit, check, and run alcy without a server.",
  "label.example": "Example",
  "label.theme": "Theme",
  "label.language": "Language",
  "theme.auto": "Auto",
  "theme.light": "Light",
  "theme.dark": "Dark",
  "button.check": "Check",
  "button.checkTitle": "Check the program",
  "button.run": "Run",
  "button.runTitle": "Compile and run",
  "button.running": "Running...",
  "compiler.idle": "compiler not loaded",
  "compiler.loading": "loading compiler...",
  "compiler.ready": "compiler ready",
  "compiler.error": "compiler unavailable",
  "tab.problems": "Problems",
  "tab.output": "Output",
  "problems.empty": "No diagnostics yet.",
  "problems.count": "diagnostics",
  "severity.error": "error",
  "severity.warning": "warning",
  "severity.note": "note",
  "samples.loading": "loading...",
  "aria.splitter": "Resize the result panel",
  "aria.editor": "Source editor",
  "aria.source": "alcy source",
  "aria.results": "Results",
  "hint.run": "Ctrl/Cmd+Enter to run",
  "status.ready": "ready",
  "status.restored": "restored the last edit",
  "status.checking": "checking...",
  "status.checkOk": "check ok · {functions} functions · {ms} ms",
  "status.checkFailed": "check failed · {count} diagnostics",
  "status.compiling": "compiling...",
  "status.running": "running...",
  "status.compileFailed": "compile failed",
  "status.runFinished": "run finished · {ms} ms",
  "status.exited": "exited with code {code} · {ms} ms",
  "status.timedOut": "timed out",
  "status.runFailed": "run failed",
  "status.compilerError": "compiler error",
  "status.compilerUnavailable": "compiler unavailable",
  "status.highlightingUnavailable": "highlighting unavailable: {detail}",
  "status.highlightingDisabled": "highlighting disabled: {detail}",
  "status.loadExampleFailed": "could not load the example: {detail}",
  "meta.compiling": "compiling...",
  "meta.compileFailed": "compile failed",
  "meta.compiled": "compiled in {ms} ms · wasm {bytes} bytes · {functions} functions",
  "meta.timedOut": "timed out",
  "meta.exitCode": "exit code {code}",
  "meta.notRunnable": "not runnable",
  "program.timedOut": "program did not finish within {seconds} s and was terminated",
  "compiler.missing":
    "the compiler wasm module could not be loaded{detail}. Build it with `uv run ./tools/site.py build` and serve the result.",
} as const;

export type MessageKey = keyof typeof en;
export type MessageParams = Record<string, string | number>;

const ja: Record<MessageKey, string> = {
  "label.example": "例",
  "site.guide": "ガイド",
  "site.playground": "プレイグラウンド",
  "site.previous": "前へ",
  "site.next": "次へ",
  "label.settings": "設定",
  "meta.guide": "alcy ガイド",
  "noscript.playground": "プレイグラウンドの実行には JavaScript が必要です。",
  "meta.playground":
    "alcy 言語のブラウザ wasm プレイグラウンド。サーバなしで編集・チェック・実行できます。",
  "label.theme": "テーマ",
  "label.language": "言語",
  "theme.auto": "自動",
  "theme.light": "ライト",
  "theme.dark": "ダーク",
  "button.check": "チェック",
  "button.checkTitle": "プログラムをチェック",
  "button.run": "実行",
  "button.runTitle": "コンパイルして実行",
  "button.running": "実行中...",
  "compiler.idle": "コンパイラ未読込",
  "compiler.loading": "コンパイラ読込中...",
  "compiler.ready": "コンパイラ準備完了",
  "compiler.error": "コンパイラ利用不可",
  "tab.problems": "問題",
  "tab.output": "出力",
  "problems.empty": "診断はまだありません。",
  "problems.count": "診断",
  "severity.error": "エラー",
  "severity.warning": "警告",
  "severity.note": "注意",
  "samples.loading": "読み込み中...",
  "aria.splitter": "結果パネルのサイズを変更",
  "aria.editor": "ソースエディタ",
  "aria.source": "alcy ソース",
  "aria.results": "結果",
  "hint.run": "Ctrl/Cmd+Enter で実行",
  "status.ready": "準備完了",
  "status.restored": "前回の編集を復元しました",
  "status.checking": "チェック中...",
  "status.checkOk": "チェック成功 · 関数 {functions} 個 · {ms} ms",
  "status.checkFailed": "チェック失敗 · 診断 {count} 件",
  "status.compiling": "コンパイル中...",
  "status.running": "実行中...",
  "status.compileFailed": "コンパイル失敗",
  "status.runFinished": "実行完了 · {ms} ms",
  "status.exited": "終了コード {code} · {ms} ms",
  "status.timedOut": "タイムアウト",
  "status.runFailed": "実行失敗",
  "status.compilerError": "コンパイラエラー",
  "status.compilerUnavailable": "コンパイラ利用不可",
  "status.highlightingUnavailable": "ハイライトを利用できません: {detail}",
  "status.highlightingDisabled": "ハイライトを停止しました: {detail}",
  "status.loadExampleFailed": "例を読み込めません: {detail}",
  "meta.compiling": "コンパイル中...",
  "meta.compileFailed": "コンパイル失敗",
  "meta.compiled": "{ms} ms でコンパイル · wasm {bytes} バイト · 関数 {functions} 個",
  "meta.timedOut": "タイムアウト",
  "meta.exitCode": "終了コード {code}",
  "meta.notRunnable": "実行不可",
  "program.timedOut": "プログラムが {seconds} 秒以内に終了しなかったため停止しました",
  "compiler.missing":
    "コンパイラの wasm モジュールを読み込めませんでした{detail}。`uv run ./tools/site.py build` でビルドしてから配信してください。",
};

export type Language = "en" | "ja";

export const LANGUAGES: Language[] = ["en", "ja"];

// The name of a language in that language, which is what a switcher
// should show.
export const LANGUAGE_NAMES: Record<Language, string> = {
  en: "English",
  ja: "日本語",
};

const CATALOGS: Record<Language, Record<MessageKey, string>> = { en, ja };

// A page's `<html lang>` names its tree; a value outside the catalog
// falls back to English so a hand-edited page still renders.
export function toLanguage(value: string): Language {
  return LANGUAGES.includes(value as Language) ? (value as Language) : "en";
}

export function translate(
  language: Language,
  key: MessageKey,
  params?: MessageParams,
): string {
  const template = CATALOGS[language][key] ?? en[key];
  if (!params) {
    return template;
  }
  return template.replace(/\{(\w+)\}/g, (match: string, name: string) =>
    name in params ? String(params[name]) : match,
  );
}

// A guard for the template stamper, so a typo in `{{t:...}}` fails the
// build instead of rendering as "undefined".
export function isMessageKey(key: string): key is MessageKey {
  return key in en;
}
