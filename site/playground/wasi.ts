// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The WASI preview1 host side for one compiled program, as a classic
// script so the runner worker can load it with `importScripts`. Keeping
// it separate from the worker also keeps it runnable outside a worker,
// which is how it is smoke-tested.
//
// The module contract is fixed by `compiler/playground/`: the program
// exports `_start` and its memory, and imports `fd_write` and `proc_exit`
// from `wasi_snapshot_preview1`.

"use strict";

interface AlcyRunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  // How long `_start` ran, in milliseconds; instantiating the module and
  // collecting the output are not part of it.
  ms: number;
}

interface AlcyWasiNamespace {
  run(buffer: BufferSource): Promise<AlcyRunResult>;
}

declare var AlcyWasi: AlcyWasiNamespace;
interface WorkerGlobalScope {
  AlcyWasi: AlcyWasiNamespace;
}

(function (global: WorkerGlobalScope) {
  "use strict";

  class ProcExit extends Error {
    readonly code: number;

    constructor(code: number) {
      super(`proc_exit(${code})`);
      this.code = code >>> 0;
    }
  }

  interface Streams {
    stdout: Uint8Array[];
    stderr: Uint8Array[];
  }

  function makeImports(
    getMemory: () => WebAssembly.Memory,
    streams: Streams,
  ): WebAssembly.Imports {
    return {
      wasi_snapshot_preview1: {
        // Collects every iovec, records the total, and reports success.
        // Bytes are decoded once at the end, so a character split across
        // two iovecs still decodes whole.
        fd_write(fd: number, iovsPtr: number, iovsLen: number, nwrittenPtr: number): number {
          const memory = getMemory();
          const view = new DataView(memory.buffer);
          const sink = fd === 2 ? streams.stderr : streams.stdout;
          let total = 0;
          for (let i = 0; i < iovsLen; i++) {
            const base = iovsPtr + i * 8;
            const ptr = view.getUint32(base, true);
            const len = view.getUint32(base + 4, true);
            sink.push(new Uint8Array(memory.buffer, ptr, len).slice());
            total += len;
          }
          view.setUint32(nwrittenPtr, total, true);
          return 0;
        },

        proc_exit(code: number): never {
          throw new ProcExit(code);
        },
      },
    };
  }

  function decode(chunks: Uint8Array[]): string {
    const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
    const all = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      all.set(chunk, offset);
      offset += chunk.length;
    }
    return new TextDecoder("utf-8").decode(all);
  }

  // Instantiates `buffer` -- wasm bytes or the module they came from -- and
  // runs `_start`. Resolves with the captured output and the exit code; a
  // program that traps rejects with the trap's error.
  async function run(buffer: BufferSource): Promise<AlcyRunResult> {
    const streams: Streams = { stdout: [], stderr: [] };
    let instance: WebAssembly.Instance | null = null;
    const imports = makeImports(() => {
      if (instance === null) {
        throw new Error("the program called its host before it was instantiated");
      }
      return instance.exports.memory as WebAssembly.Memory;
    }, streams);
    const { instance: created } = await WebAssembly.instantiate(buffer, imports);
    instance = created;
    if (!(instance.exports.memory instanceof WebAssembly.Memory)) {
      throw new Error("the module does not export its memory");
    }
    if (typeof instance.exports._start !== "function") {
      throw new Error("the module does not export _start");
    }
    let exitCode = 0;
    const started = performance.now();
    try {
      (instance.exports._start as () => void)();
    } catch (error) {
      if (error instanceof ProcExit) {
        exitCode = error.code;
      } else {
        throw error;
      }
    }
    const ms = performance.now() - started;
    return {
      exitCode,
      stdout: decode(streams.stdout),
      stderr: decode(streams.stderr),
      ms,
    };
  }

  global.AlcyWasi = { run };
})(typeof self !== "undefined" ? self : (globalThis as unknown as WorkerGlobalScope));
