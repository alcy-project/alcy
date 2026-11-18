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
  // here ; the raw layer keeps it visible.
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
  //. `None` when the end of input arrives before any byte.
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

test {
  test a_reader_holds_its_capacity_as_bytes_from_the_start() {
    r := BufReader::new(Fd::from_raw(-1), 4)
    assert(r.buf.len_bytes() == 4, "four bytes exist to read into")
    assert(r.buf.byte(0) == 0u8, "the first is zeroed")
    assert(r.buf.byte(3) == 0u8, "the last is zeroed")
    assert(r.pos == 0, "nothing is consumed")
    assert(r.len == 0, "nothing is read")
  }

  test a_zero_capacity_still_holds_one_byte() {
    r := BufReader::new(Fd::from_raw(-1), 0)
    assert(r.buf.len_bytes() == 1, "a read always has a byte to land in")
  }

  test read_byte_consumes_the_buffered_bytes_in_order() {
    mut r := buffered("ab")
    assert(next_byte(&mut r) == 0x61u8, "the first byte is a")
    assert(r.pos == 1, "one byte is consumed")
    assert(next_byte(&mut r) == 0x62u8, "the second byte is b")
    assert(r.pos == 2, "two bytes are consumed")
  }

  test peek_byte_leaves_the_position_where_it_was() {
    mut r := buffered("ab")
    assert(peeked_byte(&mut r) == 0x61u8, "the first byte is a")
    assert(peeked_byte(&mut r) == 0x61u8, "a second peek sees the same byte")
    assert(r.pos == 0, "nothing is consumed")
    assert(next_byte(&mut r) == 0x61u8, "the peeked byte is still the next one")
  }

  test a_line_drops_the_newline_that_ends_it() {
    mut r := buffered("ab\ncd\n")
    first := line(&mut r)
    assert(first.as_str() == "ab", "the newline is not part of the line")
    assert(r.pos == 3, "the reader stops after the newline")
    second := line(&mut r)
    assert(second.as_str() == "cd", "the rest of the buffer is the next line")
  }

  test a_line_drops_one_carriage_return_before_the_newline() {
    mut r := buffered("ab\r\n")
    got := line(&mut r)
    assert(got.as_str() == "ab", "the pair ends the line and neither byte is in it")
  }

  test a_line_keeps_a_carriage_return_that_is_not_the_last_byte() {
    mut r := buffered("a\r\r\n")
    got := line(&mut r)
    assert(got.as_str() == "a\r", "only one carriage return goes with the newline")
  }

  test an_empty_line_is_a_line() {
    mut r := buffered("\n")
    got := line(&mut r)
    assert(got.len_bytes() == 0, "the line is there and empty")
  }

  // A reader whose buffer holds `bytes`: the buffered part is read
  // without a descriptor, so `fill` is never reached.
  fn buffered(bytes: str) -> BufReader {
    n := bytes.len_bytes()
    buf := String::from(bytes)
    ret BufReader { fd: Fd::from_raw(-1), buf: buf, pos: 0, len: n }
  }

  fn next_byte(r: &mut BufReader) -> u8 {
    mut out := 0u8
    match r.read_byte() {
      Result::Ok(v) => {
        match v {
          Option::Some(b) => {
            out = b
          }
          Option::None => {
            panic("a buffered byte was expected")
          }
        }
      }
      Result::Err(_) => {
        panic("a buffered byte needs no read")
      }
    }
    ret out
  }

  fn peeked_byte(r: &mut BufReader) -> u8 {
    mut out := 0u8
    match r.peek_byte() {
      Result::Ok(v) => {
        match v {
          Option::Some(b) => {
            out = b
          }
          Option::None => {
            panic("a buffered byte was expected")
          }
        }
      }
      Result::Err(_) => {
        panic("a buffered byte needs no read")
      }
    }
    ret out
  }

  fn line(r: &mut BufReader) -> String {
    match r.read_line() {
      Result::Ok(v) => {
        match v {
          Option::Some(s) => {
            ret s
          }
          Option::None => {
            panic("a buffered line was expected")
          }
        }
      }
      Result::Err(_) => {
        panic("a buffered line needs no read")
      }
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
