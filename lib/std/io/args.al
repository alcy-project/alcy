// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: the process arguments.

// The number of arguments, the program name first.
pub fn args_len() -> usize {
  ret unsafe { arg_count() } as usize
}

// The argument at `i` as a vector of bytes, or `None` when the index
// is past the end. The program name is index zero. The bytes are
// copied out, so the result is this program's own.
pub fn arg_at(i: usize) -> Option<Vec<u8>> {
  if (i as i64) >= unsafe { arg_count() } {
    ret Option::None
  }
  bytes := unsafe { arg(i as i64) }
  ret Option::Some(Vec::<u8>::from_slice(bytes))
}

// The argument list by value, the program name first.
pub fn args() -> Vec<Vec<u8>> {
  n := args_len()
  mut out := Vec::<Vec<u8>>::with_capacity(n)
  mut i := 0 as usize
  while i < n {
    out.push(arg_at(i).or_panic("the count is the argument count"))
    i = i + 1
  }
  ret out
}

// The same argument checked as text: every byte must spell scalars,
// or the argument is refused.
pub fn arg_at_str(i: usize) -> Option<String> {
  match arg_at(i) {
    Option::Some(bytes) => {
      match String::from_slice(bytes.as_slice()) {
        Option::Some(text) => {
          ret Option::Some(text)
        }
        Option::None => {
          ret Option::None
        }
      }
    }
    Option::None => {
      ret Option::None
    }
  }
}

// The whole argument list checked as text, or `None` when any argument
// is not well-formed UTF-8.
pub fn args_str() -> Option<Vec<String>> {
  n := args_len()
  mut out := Vec::<String>::with_capacity(n)
  mut i := 0 as usize
  while i < n {
    match arg_at_str(i) {
      Option::Some(text) => {
        out.push(text)
      }
      Option::None => {
        ret Option::None
      }
    }
    i = i + 1
  }
  ret Option::Some(out)
}
