// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The slice of Node the site generator uses. The repository installs no
// `@types/node`; naming the surface here follows the playground's
// hand-written `vendor/web-tree-sitter.d.ts`, and keeps the generator's
// dependencies pinned to what `config.toml` already lists.

declare module "node:fs" {
  export interface Dirent {
    readonly name: string;
    isDirectory(): boolean;
    isFile(): boolean;
  }

  export function readFileSync(path: string, encoding: "utf8"): string;
  export function readFileSync(path: string): Uint8Array;
  export function writeFileSync(path: string, data: string): void;
  export function mkdirSync(path: string, options?: { recursive?: boolean }): void;
  export function readdirSync(path: string, options: { withFileTypes: true }): Dirent[];
  export function existsSync(path: string): boolean;
}

declare module "node:path" {
  export interface PathModule {
    join(...parts: string[]): string;
    dirname(path: string): string;
    basename(path: string, suffix?: string): string;
    resolve(...parts: string[]): string;
    relative(from: string, to: string): string;
    normalize(path: string): string;
  }

  export const posix: PathModule;
  export const join: PathModule["join"];
  export const dirname: PathModule["dirname"];
  export const basename: PathModule["basename"];
  export const resolve: PathModule["resolve"];
  export const relative: PathModule["relative"];
}

declare module "node:url" {
  export function pathToFileURL(path: string): { href: string };
}

declare module "node:test" {
  export function test(name: string, fn: () => void | Promise<void>): void;
}

declare module "node:assert/strict" {
  export function equal(actual: unknown, expected: unknown, message?: string): void;
  export function deepEqual(actual: unknown, expected: unknown, message?: string): void;
  export function match(value: string, pattern: RegExp, message?: string): void;
  export function ok(value: unknown, message?: string): void;
  export function throws(fn: () => unknown, message?: string): void;
}

declare const process: {
  argv: string[];
  exitCode: number;
  exit(code?: number): never;
  stdout: { write(text: string): void };
  stderr: { write(text: string): void };
};
