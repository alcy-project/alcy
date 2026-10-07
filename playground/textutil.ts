// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Offset conversion between the UTF-8 bytes the compiler reports and the
// UTF-16 code units a DOM string uses. Diagnostics carry byte offsets,
// while selection ranges are code-unit indices, and a non-ASCII character
// makes the two differ.

// Returns a table where `table[byteOffset]` is the code-unit index at or
// before that byte. The last entry is the text's length in code units, so
// `table` has `utf8Length(text) + 1` entries.
export function byteToUtf16Map(text: string): Uint32Array {
  const table: number[] = [];
  let utf16 = 0;
  for (const ch of text) {
    const codePoint = ch.codePointAt(0) ?? 0;
    const bytes =
      codePoint < 0x80 ? 1 : codePoint < 0x800 ? 2 : codePoint < 0x10000 ? 3 : 4;
    for (let i = 0; i < bytes; i++) {
      table.push(utf16);
    }
    utf16 += ch.length;
  }
  table.push(utf16);
  return Uint32Array.from(table);
}

export function utf16IndexAtByte(table: Uint32Array, byteOffset: number): number {
  if (byteOffset <= 0) {
    return 0;
  }
  if (byteOffset >= table.length) {
    return table[table.length - 1] ?? 0;
  }
  return table[byteOffset] ?? 0;
}
