// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The page-side wrapper over the playground's C ABI: load the built
// launcher, call `alcy_check`/`alcy_compile` with a source string, get
// plain objects back, and run the module a compile produced.
//
//   import factory from "./alcy_playground.js";
//   import { createPlayground } from "./alcy.mjs";
//
//   const alcy = await createPlayground({ factory });
//   const result = alcy.compile("fn main() { println(\"hi\") }");
//   const { stdout, exitCode } = await alcy.run(result.wasm);
//
// `run` implements only the two imports a produced module declares
// (`fd_write` and `proc_exit`), so a page needs no WASI runtime behind
// it. It is the module's own host, not a general-purpose one.

// The `AlcyResult` layout on wasm32, as 32-bit words. `playground.h` is
// the definition; this table is the one place the JS side names it.
const RESULT = {
  wasm: 0,
  wasm_len: 1,
  diagnostics: 2,
  diagnostics_len: 3,
  file_count: 4,
  module_count: 5,
  function_count: 6,
  ok: 7,
};

const RESULT_WORDS = 8;

// Thrown by `proc_exit` to unwind a module that ends by exiting, which
// is every module with an `i32` main.
const EXITED = Symbol("exited");

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// The result struct is heap memory the compiler filled; copy what it
// answers into JS values and release the C side before returning.
function takeResult(Module, structPtr) {
  const word = (field) => Module.HEAPU32[(structPtr >> 2) + RESULT[field]];
  const slice = (ptr, len) =>
    len === 0 ? null : Module.HEAPU8.slice(ptr, ptr + len);
  const result = {
    ok: word("ok") === 1,
    wasm: slice(word("wasm"), word("wasm_len")),
    fileCount: word("file_count"),
    moduleCount: word("module_count"),
    functionCount: word("function_count"),
    diagnostics: [],
  };
  const diagnosticsPtr = word("diagnostics");
  const diagnosticsLen = word("diagnostics_len");
  if (diagnosticsLen !== 0) {
    const text = decoder.decode(
      new Uint8Array(Module.HEAPU8.buffer, diagnosticsPtr, diagnosticsLen),
    );
    result.diagnostics = JSON.parse(text);
  }
  Module._alcy_release(structPtr);
  return result;
}

// One call of an exported C function over a JS string.
function call(Module, name, source) {
  const text = encoder.encode(source);
  const sourcePtr = Module._malloc(Math.max(text.length, 1));
  const structPtr = Module._malloc(RESULT_WORDS * 4);
  Module.HEAPU8.set(text, sourcePtr);
  Module[`_${name}`](sourcePtr, text.length, structPtr);
  Module._free(sourcePtr);
  return takeResult(Module, structPtr);
}

// Runs a module the compiler produced: instantiate with the two imports
// it declares, call `_start`, and collect its stdout and stderr.
async function run(wasmBytes) {
  let memory = null;
  let exitCode = 0;
  let stdout = "";
  let stderr = "";
  const imports = {
    wasi_snapshot_preview1: {
      fd_write: (fd, iovs, iovsLen, writtenPtr) => {
        const view = new DataView(memory.buffer);
        let written = 0;
        for (let i = 0; i < iovsLen; i++) {
          const ptr = view.getUint32(iovs + i * 8, true);
          const len = view.getUint32(iovs + i * 8 + 4, true);
          const text = decoder.decode(new Uint8Array(memory.buffer, ptr, len));
          if (fd === 1) {
            stdout += text;
          } else if (fd === 2) {
            stderr += text;
          }
          written += len;
        }
        view.setUint32(writtenPtr, written, true);
        return 0;
      },
      proc_exit: (code) => {
        exitCode = code;
        throw EXITED;
      },
    },
  };
  const { instance } = await WebAssembly.instantiate(wasmBytes, imports);
  memory = instance.exports.memory;
  try {
    instance.exports._start();
  } catch (error) {
    if (error !== EXITED) {
      throw error;
    }
  }
  return { stdout, stderr, exitCode };
}

// Loads a built playground launcher and binds its ABI.
//
// `factory` is the launcher's own factory function, or a promise of the
// module: the emscripten `-sMODULARIZE` output in either role. A caller
// that wants a specific instance passes it; the common case imports the
// launcher and passes `factory` itself.
export async function createPlayground({ factory }) {
  const Module = await factory();
  return {
    check: (source) => call(Module, "alcy_check", source),
    compile: (source) => call(Module, "alcy_compile", source),
    run,
  };
}
