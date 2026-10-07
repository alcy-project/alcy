// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The part of the tree-sitter web binding `highlight.ts` uses. The
// upstream declaration wraps everything in `declare module
// "web-tree-sitter"` and refers to emscripten's types, so a relative
// import of the vendored file cannot reach it; the surface the page uses
// is small enough to name here instead.

export interface ParserOptions {
  wasmBinary?: ArrayBuffer;
  locateFile?(path: string, scriptDirectory: string): string;
}

export interface Node {
  startIndex: number;
  endIndex: number;
}

export interface Tree {
  rootNode: Node;
  delete(): void;
}

export interface QueryCapture {
  name: string;
  node: Node;
}

export class Language {
  static load(input: string | Uint8Array): Promise<Language>;
}

export class Query {
  constructor(language: Language, source: string);
  captures(node: Node): QueryCapture[];
  delete(): void;
}

export class Parser {
  static init(options?: ParserOptions): Promise<void>;
  setLanguage(language: Language): void;
  parse(text: string): Tree;
  delete(): void;
}
