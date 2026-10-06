// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The WASI preview1 host side for one compiled program, as a classic
// script so the runner worker can load it with `importScripts`. Keeping
// it separate from the worker also keeps it runnable under node, which
// is how it is smoke-tested.
//
// The module contract is fixed by `docs/adr/0049`: the program exports
// `_start` and its memory, and imports `fd_write` and `proc_exit` from
// `wasi_snapshot_preview1`.

(function (global) {
  "use strict";

  class ProcExit extends Error {
    constructor(code) {
      super(`proc_exit(${code})`);
      this.code = code >>> 0;
    }
  }

  function makeImports(getMemory, streams) {
    return {
      wasi_snapshot_preview1: {
        // Collects every iovec, records the total, and reports success.
        // Bytes are decoded once at the end, so a character split across
        // two iovecs still decodes whole.
        fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
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

        proc_exit(code) {
          throw new ProcExit(code);
        },
      },
    };
  }

  function decode(chunks) {
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
  async function run(buffer) {
    const streams = { stdout: [], stderr: [] };
    let instance = null;
    const imports = makeImports(() => instance.exports.memory, streams);
    const { instance: created } = await WebAssembly.instantiate(buffer, imports);
    instance = created;
    if (!(instance.exports.memory instanceof WebAssembly.Memory)) {
      throw new Error("the module does not export its memory");
    }
    if (typeof instance.exports._start !== "function") {
      throw new Error("the module does not export _start");
    }
    let exitCode = 0;
    try {
      instance.exports._start();
    } catch (error) {
      if (error instanceof ProcExit) {
        exitCode = error.code;
      } else {
        throw error;
      }
    }
    return {
      exitCode,
      stdout: decode(streams.stdout),
      stderr: decode(streams.stderr),
    };
  }

  global.AlcyWasi = { run };
})(typeof self !== "undefined" ? self : globalThis);
