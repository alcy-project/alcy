// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The environment the process inherited. `env` answers the raw bytes
// the platform stored for a name, and `env_str` checks them as UTF-8
// first. A variable that holds no bytes is still set: `env` says
// `Some` of an empty vector.

pub unsafe intrinsic fn environ_exists(name: &[u8]) -> i32;

pub unsafe intrinsic fn environ_value(name: &[u8]) -> &[u8];

// The bytes of the variable called `name`, or `None` when it is not
// set. The bytes are the platform's own spelling: on Unix they are
// whatever the kernel stored; on Windows they are the UTF-8
// encoding of the wide value.
pub fn env(name: str) -> Option<Vec<u8>> {
  bytes := String::from(name).as_bytes()
  if unsafe { environ_exists(bytes) } == 1 {
    ret Option::Some(Vec::<u8>::from_slice(unsafe { environ_value(bytes) }))
  }
  ret Option::None
}

// The value of the variable called `name`, checked as UTF-8, or
// `None` when it is not set or its bytes are not UTF-8.
pub fn env_str(name: str) -> Option<String> {
  match env(name) {
    Option::Some(bytes) => {
      ret String::from_slice(bytes.as_slice())
    }
    Option::None => {
      ret Option::None
    }
  }
}

#[when(testing)] {
  #[test] fn a_variable_the_process_has_is_found() {
    match env_str("ALCY_STD_TEST_ENV") {
      Option::Some(value) => {
        assert(value.as_str() == "alcy-std-test-value", "the value arrives as bytes")
      }
      Option::None => {
        panic("the test environment sets ALCY_STD_TEST_ENV")
      }
    }
  }

  #[test] fn a_variable_the_process_does_not_have_is_none() {
    assert(env_str("ALCY_STD_ENV_NOT_SET").is_none(), "a missing variable is not there")
  }

  #[test] fn env_and_env_str_agree() {
    match env("ALCY_STD_TEST_ENV") {
      Option::Some(bytes) => {
        assert(bytes.len() == 19, "alcy-std-test-value is 19 bytes")
      }
      Option::None => {
        panic("the test environment sets ALCY_STD_TEST_ENV")
      }
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
