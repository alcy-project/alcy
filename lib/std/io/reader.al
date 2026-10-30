// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: reading bytes through a buffer over a descriptor.

use sys::raw::read;

// The capacity a reader starts with: a token or a line rarely needs
// more than one read behind it.
const DEFAULT_CAPACITY: usize = 8192

// A reader over a descriptor. It owns its buffer and not the
// descriptor, so the standard input can be wrapped and never closed;
// `fill` performs one underlying read and reports it as itself
// (ADR-0129).
pub struct BufReader {
  fd: Fd,
  buf: String,
  pos: usize,
  len: usize,
}

impl BufReader {
  // The standard input, buffered; the descriptor belongs to the
  // process.
  pub fn stdin() -> BufReader {
    ret BufReader::new(Fd::stdin(), DEFAULT_CAPACITY)
  }

  pub fn new(fd: Fd, capacity: usize) -> BufReader {
    mut cap := capacity
    if cap == 0 {
      cap = 1
    }
    // The buffer's bytes exist from the start, so a read borrows a
    // real slice and no uninitialized storage crosses the boundary.
    mut buf := String::with_capacity(cap)
    mut i := 0 as usize
    while i < cap {
      buf.push(0u8)
      i = i + 1
    }
    ret BufReader { fd: fd, buf: buf, pos: 0, len: 0 }
  }

  // One read(2) into the buffer, answering the bytes gained; zero is
  // the end of input. An interrupted call is retried here, and only
  // here (ADR-0129); the raw layer keeps it visible.
  pub fn fill(mut self: &mut Self) -> Result<usize, OsError> {
    self.pos = 0
    self.len = 0
    loop {
      match read(self.fd, self.buf.as_mut_slice()) {
        Result::Ok(n) => {
          self.len = n
          ret Result::Ok(n)
        }
        Result::Err(e) => {
          match e.kind {
            ErrorKind::Interrupted => {
              // Nothing was consumed; call again.
            }
            _ => {
              ret Result::Err(e)
            }
          }
        }
      }
    }
  }

  // The next byte, or `None` at the end of input.
  pub fn read_byte(mut self: &mut Self) -> Result<Option<u8>, OsError> {
    if self.pos >= self.len {
      n := match self.fill() {
        Result::Ok(v) => v,
        Result::Err(e) => {
          ret Result::Err(e)
        }
      }
      if n == 0 {
        ret Result::Ok(Option::None)
      }
    }
    b := self.buf.byte(self.pos)
    self.pos = self.pos + 1
    ret Result::Ok(Option::Some(b))
  }

  // The next byte without consuming it, or `None` at the end.
  pub fn peek_byte(mut self: &mut Self) -> Result<Option<u8>, OsError> {
    if self.pos >= self.len {
      n := match self.fill() {
        Result::Ok(v) => v,
        Result::Err(e) => {
          ret Result::Err(e)
        }
      }
      if n == 0 {
        ret Result::Ok(Option::None)
      }
    }
    ret Result::Ok(Option::Some(self.buf.byte(self.pos)))
  }

  // One line, with one trailing `\n` (and one `\r` before it)
  // dropped. The bytes are the text and nothing is validated
  // (ADR-0091). `None` when the end of input arrives before any byte.
  pub fn read_line(mut self: &mut Self) -> Result<Option<String>, OsError> {
    mut out := String::with_capacity(64)
    mut got := false
    loop {
      next_byte := match self.read_byte() {
        Result::Ok(v) => v,
        Result::Err(e) => {
          ret Result::Err(e)
        }
      }
      match next_byte {
        Option::Some(b) => {
          got = true
          if b == 0x0au8 {
            break
          }
          out.push(b)
        }
        Option::None => {
          break
        }
      }
    }
    if !got {
      ret Result::Ok(Option::None)
    }
    if out.len_bytes() > 0 && out.byte(out.len_bytes() - 1) == 0x0du8 {
      _ := out.pop_byte()
    }
    ret Result::Ok(Option::Some(out))
  }

  // Everything that is left, as bytes.
  pub fn read_to_end(mut self: &mut Self) -> Result<Vec<u8>, OsError> {
    mut out := Vec::<u8>::with_capacity(64)
    mut done := false
    while !done {
      next_byte := match self.read_byte() {
        Result::Ok(v) => v,
        Result::Err(e) => {
          ret Result::Err(e)
        }
      }
      match next_byte {
        Option::Some(b) => {
          out.push(b)
        }
        Option::None => {
          done = true
        }
      }
    }
    ret Result::Ok(out)
  }
}
