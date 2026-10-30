// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: the token domain over a buffered reader (ADR-0129).

// The failure a read reports: the end of input arrived where a token
// was expected, a token did not parse (it is carried), or the
// operating system refused.
pub enum ScanError {
  End,
  Parse(String),
  Io(OsError),
}

// ASCII space, tab, CR, and LF separate tokens.
fn is_space(b: u8) -> bool {
  ret b == 0x20u8 || b == 0x09u8 || b == 0x0du8 || b == 0x0au8
}

// The token cursor. The reader is owned and the scratch string is
// reused, so a typed read allocates only when the token outgrows the
// scratch.
pub struct Scanner {
  reader: BufReader,
  token: String,
}

impl Scanner {
  // The standard input.
  pub fn stdin() -> Scanner {
    ret Scanner::over(BufReader::stdin())
  }

  pub fn over(reader: BufReader) -> Scanner {
    ret Scanner { reader: reader, token: String::with_capacity(64) }
  }

  // The next token, copied out; `End` when the input runs out first.
  pub fn next_token(mut self: &mut Self) -> Result<String, ScanError> {
    match self.read_token() {
      Result::Ok(true) => {}
      Result::Ok(false) => {
        ret Result::Err(ScanError::End)
      }
      Result::Err(e) => {
        ret Result::Err(ScanError::Io(e))
      }
    }
    mut copy := String::with_capacity(self.token.len_bytes())
    copy.push_str(self.token.as_str())
    ret Result::Ok(copy)
  }

  // The next token parsed as `T`; the end of input before a token is
  // `End`, and a token that does not parse names itself.
  pub fn next<T: Parse>(mut self: &mut Self) -> Result<T, ScanError> {
    match self.read_token() {
      Result::Ok(true) => {}
      Result::Ok(false) => {
        ret Result::Err(ScanError::End)
      }
      Result::Err(e) => {
        ret Result::Err(ScanError::Io(e))
      }
    }
    match T::parse(self.token.as_str()) {
      Option::Some(v) => {
        ret Result::Ok(v)
      }
      Option::None => {
        mut found := String::with_capacity(self.token.len_bytes())
        found.push_str(self.token.as_str())
        ret Result::Err(ScanError::Parse(found))
      }
    }
  }

  // `count` tokens parsed as `T`, in order; the end of input before
  // the count is met answers `End`.
  pub fn next_vec<T: Parse>(mut self: &mut Self, count: usize) -> Result<Vec<T>, ScanError> {
    mut out := Vec::<T>::with_capacity(count)
    mut i := 0 as usize
    while i < count {
      match self.next::<T>() {
        Result::Ok(v) => {
          out.push(v)
        }
        Result::Err(e) => {
          ret Result::Err(e)
        }
      }
      i = i + 1
    }
    ret Result::Ok(out)
  }

  // One line from the same input, for the line-oriented mix.
  pub fn next_line(mut self: &mut Self) -> Result<Option<String>, OsError> {
    ret self.reader.read_line()
  }

  // Fills `self.token` with the next token, answering whether one was
  // read. Leading whitespace is consumed, and the terminating
  // whitespace with it.
  fn read_token(mut self: &mut Self) -> Result<bool, OsError> {
    self.token.truncate(0)
    mut started := false
    loop {
      next_byte := match self.reader.read_byte() {
        Result::Ok(v) => v,
        Result::Err(e) => {
          ret Result::Err(e)
        }
      }
      match next_byte {
        Option::Some(b) => {
          if is_space(b) {
            if started {
              ret Result::Ok(true)
            }
          } else {
            started = true
            self.token.push(b)
          }
        }
        Option::None => {
          ret Result::Ok(started)
        }
      }
    }
  }
}
