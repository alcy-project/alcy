// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use core::string::str_byte;
use core::string::str_len;
use fmt::display::comp_pieces;
use fmt::display::Display;

// `alcy/std/fmt`: formatting into a caller's buffer.

pub struct WriteOutcome {
  pub written: usize,
  pub total: usize,
}

pub fn write(comp fmt: str, buf: &mut [u8], args: ..) -> WriteOutcome {
  mut off := 0usize
  mut total := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      mut n := 0usize
      comp if p.precision == 255 {
        n = args[p.arg].display(buf[off..])
      } else {
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        n = args[p.arg].display_precision(prec, trim, buf[off..])
      }
      total += n
      if off < buf.len() {
        mut room := buf.len() - off
        if n < room {
          room = n
        }
        off += room
      }
    } else {
      lit := p.literal
      n := str_len(lit)
      total += n
      mut i := 0usize
      while i < n && off < buf.len() {
        buf[off] = str_byte(lit, i)
        off += 1
        i += 1
      }
    }
  }
  ret WriteOutcome { written: off, total: total }
}
