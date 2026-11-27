// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: writing to the standard output.
//
// Each print wrapper is an expansion: it formats its arguments into an
// owned string and writes that string to the named descriptor.

use core::string::str_byte;
use core::string::str_len;
use fmt::display::comp_pieces;
use fmt::display::Display;
use fmt::display::DisplayPrecision;
use sys::fd::Fd;
use sys::raw::write;
use text::string::String;

fn emit(fd: Fd, text: str) {
  _ := write(fd, text).or_panic("writing stdout or stderr failed")
}

pub fn print(comp fmt: str, args: ..) {
  mut total := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        mut empty := [0u8; 0]
        total += args[p.arg].display(&mut empty[..])
      } else {
        mut empty := [0u8; 0]
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        total += args[p.arg].display_precision(prec, trim, &mut empty[..])
      }
    } else {
      total += str_len(p.literal)
    }
  }
  mut out := String::with_capacity(total)
  mut off := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        off += args[p.arg].display(unsafe { out.spare_capacity_mut() } [off..])
      } else {
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        off +=
          args[p.arg]
          .display_precision(prec, trim, unsafe { out.spare_capacity_mut() } [off..])
      }
    } else {
      lit := p.literal
      n := str_len(lit)
      mut i := 0usize
      spare := unsafe { out.spare_capacity_mut() }
      while i < n {
        spare[off + i] = str_byte(lit, i)
        i += 1
      }
      off += n
    }
  }
  unsafe { out.set_len(off) }
  emit(Fd::stdout(), out.as_str())
}

pub fn println(comp fmt: str, args: ..) {
  mut total := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        mut empty := [0u8; 0]
        total += args[p.arg].display(&mut empty[..])
      } else {
        mut empty := [0u8; 0]
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        total += args[p.arg].display_precision(prec, trim, &mut empty[..])
      }
    } else {
      total += str_len(p.literal)
    }
  }
  mut out := String::with_capacity(total)
  mut off := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        off += args[p.arg].display(unsafe { out.spare_capacity_mut() } [off..])
      } else {
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        off +=
          args[p.arg]
          .display_precision(prec, trim, unsafe { out.spare_capacity_mut() } [off..])
      }
    } else {
      lit := p.literal
      n := str_len(lit)
      mut i := 0usize
      spare := unsafe { out.spare_capacity_mut() }
      while i < n {
        spare[off + i] = str_byte(lit, i)
        i += 1
      }
      off += n
    }
  }
  unsafe { out.set_len(off) }
  out.push_str("\n")
  emit(Fd::stdout(), out.as_str())
}

pub fn eprint(comp fmt: str, args: ..) {
  mut total := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        mut empty := [0u8; 0]
        total += args[p.arg].display(&mut empty[..])
      } else {
        mut empty := [0u8; 0]
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        total += args[p.arg].display_precision(prec, trim, &mut empty[..])
      }
    } else {
      total += str_len(p.literal)
    }
  }
  mut out := String::with_capacity(total)
  mut off := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        off += args[p.arg].display(unsafe { out.spare_capacity_mut() } [off..])
      } else {
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        off +=
          args[p.arg]
          .display_precision(prec, trim, unsafe { out.spare_capacity_mut() } [off..])
      }
    } else {
      lit := p.literal
      n := str_len(lit)
      mut i := 0usize
      spare := unsafe { out.spare_capacity_mut() }
      while i < n {
        spare[off + i] = str_byte(lit, i)
        i += 1
      }
      off += n
    }
  }
  unsafe { out.set_len(off) }
  emit(Fd::stderr(), out.as_str())
}

pub fn eprintln(comp fmt: str, args: ..) {
  mut total := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        mut empty := [0u8; 0]
        total += args[p.arg].display(&mut empty[..])
      } else {
        mut empty := [0u8; 0]
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        total += args[p.arg].display_precision(prec, trim, &mut empty[..])
      }
    } else {
      total += str_len(p.literal)
    }
  }
  mut out := String::with_capacity(total)
  mut off := 0usize
  comp for p in comp_pieces(fmt) {
    comp if p.is_arg {
      comp if p.precision == 255 {
        off += args[p.arg].display(unsafe { out.spare_capacity_mut() } [off..])
      } else {
        mut prec := p.precision
        mut trim := false
        if prec == 255 {
          prec = 6
          trim = true
        }
        off +=
          args[p.arg]
          .display_precision(prec, trim, unsafe { out.spare_capacity_mut() } [off..])
      }
    } else {
      lit := p.literal
      n := str_len(lit)
      mut i := 0usize
      spare := unsafe { out.spare_capacity_mut() }
      while i < n {
        spare[off + i] = str_byte(lit, i)
        i += 1
      }
      off += n
    }
  }
  unsafe { out.set_len(off) }
  out.push_str("\n")
  emit(Fd::stderr(), out.as_str())
}
