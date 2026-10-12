// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/collections`: a set of `str` keys.

// A set of strings, backed by `Map`. Keys are copied and hashed the
// same way the map does it, and `insert` reports whether the key was
// new. See `map.al` for the table.
pub struct Set {
  map: Map<u8>
}

impl Set {
  pub fn new() -> Set {
    ret Set { map: Map::<u8>::new() }
  }

  // Reserves room for `n` keys without changing the length.
  pub fn with_capacity(n: usize) -> Set {
    ret Set { map: Map::<u8>::with_capacity(n) }
  }

  pub fn len(self: &Self) -> usize {
    ret self.map.len()
  }

  pub fn is_empty(self: &Self) -> bool {
    ret self.map.is_empty()
  }

  pub fn capacity(self: &Self) -> usize {
    ret self.map.capacity()
  }

  // Adds `key`; true when it was not already a member.
  pub fn insert(mut self: &mut Self, key: str) -> bool {
    ret self.map.insert(key, 0 as u8).is_none()
  }

  pub fn contains(self: &Self, key: str) -> bool {
    ret self.map.contains(key)
  }

  // Removes `key`; true when it was a member.
  pub fn remove(mut self: &mut Self, key: str) -> bool {
    ret self.map.remove(key).is_some()
  }

  pub fn clear(mut self: &mut Self) {
    self.map.clear()
  }

  // Keys end when the set does; `iter` hands them out while it lives.
  pub fn iter(self: &Self) -> MapKeys<u8> {
    ret self.map.keys()
  }
}

// The backing map's equality is the set's: same length and every key of
// one present in the other.
fn maps_equal(a: &Map<u8>, b: &Map<u8>) -> bool {
  ret *a == *b
}

impl PartialEq for Set {
  fn eq(self: &Self, other: &Self) -> bool {
    ret maps_equal(&self.map, &other.map)
  }
}

impl Eq for Set {
}
