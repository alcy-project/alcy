// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The elements the shell drives, looked up once. The id is the contract
// between this table and the markup every page carries; a missing one
// throws here, at startup.

function mustGet<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (node === null) {
    throw new Error(`the page is missing #${id}`);
  }
  return node as T;
}

export const elements = {
  theme: mustGet<HTMLButtonElement>("theme"),
};
