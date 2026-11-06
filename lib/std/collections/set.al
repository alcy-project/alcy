// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/collections`: a set of `str` keys.

// A set of strings, backed by `Map`. Keys are copied and hashed the
// same way the map does it, and `insert` reports whether the key was
// new. See `map.al` for the table.
pub struct Set {
  map: Map<u8>,
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

  // Moves every key of `other` in, ending it. Each key is copied the
  // way `insert` copies one.
  pub fn extend(mut self: &mut Self, other: Set) {
    for key in other.iter() {
      _ := self.insert(key)
    }
  }

  // Whether every key of this set is also in `other`; the empty set
  // is a subset of any.
  pub fn is_subset(self: &Self, other: &Set) -> bool {
    for key in self.iter() {
      if !other.contains(key) {
        ret false
      }
    }
    ret true
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

  // A new set with every key of either. Each key is copied the way
  // `insert` copies one.
  pub fn union_with(self: &Self, other: &Self) -> Set {
    mut out := Set::with_capacity(self.len() + other.len())
    for key in self.iter() {
      _ := out.insert(key)
    }
    for key in other.iter() {
      _ := out.insert(key)
    }
    ret out
  }

  // A new set with only the keys both hold.
  pub fn intersection(self: &Self, other: &Self) -> Set {
    mut out := Set::new()
    for key in self.iter() {
      if other.contains(key) {
        _ := out.insert(key)
      }
    }
    ret out
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

// Tests for the set: one entry per property `Set` promises.
test {
  test a_new_set_is_empty() {
    s := Set::new()
    assert(s.is_empty(), "a set that was never inserted into holds nothing")
    assert(s.len() == 0, "a set that was never inserted into is of length zero")
    assert(!s.contains("alpha"), "an empty set has no key to find")
  }

  test insert_of_a_new_key_reports_it() {
    mut s := Set::new()
    assert(s.insert("alpha"), "the key was not a member before")
    assert(s.len() == 1, "the insert added one member")
    assert(s.contains("alpha"), "the member is there afterwards")
  }

  test a_duplicate_insert_reports_false_and_adds_nothing() {
    mut s := Set::new()
    _ := s.insert("alpha")
    assert(!s.insert("alpha"), "a key inserted twice is still in the set")
    assert(s.len() == 1, "the duplicate adds no member")
  }

  test contains_holds_only_the_inserted_keys() {
    mut s := Set::new()
    _ := s.insert("alpha")
    _ := s.insert("beta")
    assert(s.contains("beta"), "an inserted key is a member")
    assert(!s.contains("gamma"), "a key never inserted is not a member")
    assert(s.len() == 2, "two inserts are two members")
  }

  test remove_of_a_present_key_reports_true() {
    mut s := Set::new()
    _ := s.insert("alpha")
    _ := s.insert("beta")
    assert(s.remove("alpha"), "the key was a member")
    assert(!s.contains("alpha"), "the removed key is gone")
    assert(s.contains("beta"), "the other member is untouched")
    assert(s.len() == 1, "the removal shortens the set")
  }

  test remove_of_an_absent_key_reports_false() {
    mut s := Set::new()
    _ := s.insert("alpha")
    assert(!s.remove("beta"), "a key never inserted cannot be removed")
    assert(s.len() == 1, "the failed removal keeps the length")
    assert(s.contains("alpha"), "the failed removal keeps the member")
  }

  test clear_drops_every_member() {
    mut s := Set::with_capacity(8)
    _ := s.insert("alpha")
    _ := s.insert("beta")
    reserved := s.capacity()
    s.clear()
    assert(s.is_empty(), "clearing leaves no member")
    assert(s.capacity() == reserved, "the room reserved before the clear is kept")
    assert(!s.contains("alpha"), "a cleared key is not a member")
    _ := s.insert("gamma")
    assert(s.contains("gamma"), "the cleared set takes a new key")
  }

  test union_holds_every_key_of_either_set() {
    mut a := Set::new()
    _ := a.insert("alpha")
    _ := a.insert("beta")
    mut b := Set::new()
    _ := b.insert("beta")
    _ := b.insert("gamma")
    u := a.union_with(&b)
    assert(u.len() == 3, "the shared key is one member of the union")
    assert(u.contains("alpha"), "a key of the left set is in the union")
    assert(u.contains("gamma"), "a key of the right set is in the union")
    assert(!u.contains("delta"), "a key neither set holds is not in the union")
  }

  test intersection_holds_only_the_shared_keys() {
    mut a := Set::new()
    _ := a.insert("alpha")
    _ := a.insert("beta")
    mut b := Set::new()
    _ := b.insert("beta")
    _ := b.insert("gamma")
    i := a.intersection(&b)
    assert(i.len() == 1, "one key is in both sets")
    assert(i.contains("beta"), "the shared key is the one in the intersection")
    assert(!i.contains("alpha"), "a key only the left set holds is not in the intersection")
    assert(a.len() == 2, "the intersection leaves the left set alone")
    assert(b.len() == 2, "the intersection leaves the right set alone")
  }

  test a_set_is_a_subset_of_the_one_that_holds_more() {
    mut small := Set::new()
    _ := small.insert("alpha")
    mut large := Set::new()
    _ := large.insert("alpha")
    _ := large.insert("beta")
    assert(small.is_subset(&large), "every key of the smaller set is in the larger")
    assert(!large.is_subset(&small), "the larger set holds a key the smaller one does not")
    assert(small.is_subset(&small), "a set is a subset of itself")
  }

  test the_empty_set_is_a_subset_of_any_set() {
    empty := Set::new()
    mut s := Set::new()
    _ := s.insert("alpha")
    assert(empty.is_subset(&s), "a set with no keys is a subset of one with keys")
    assert(empty.is_subset(&empty), "a set with no keys is a subset of itself")
  }

  test sets_with_the_same_keys_are_equal() {
    mut a := Set::new()
    _ := a.insert("alpha")
    _ := a.insert("beta")
    mut b := Set::new()
    _ := b.insert("beta")
    _ := b.insert("alpha")
    assert(a == b, "the same keys make equal sets whatever order they were inserted in")
    _ := b.insert("gamma")
    assert(!(a == b), "an extra key makes the sets differ")
  }

  test extend_adds_every_key_of_the_other_set() {
    mut a := Set::new()
    _ := a.insert("alpha")
    mut b := Set::new()
    _ := b.insert("alpha")
    _ := b.insert("beta")
    a.extend(b)
    assert(a.len() == 2, "the shared key is one member")
    assert(a.contains("beta"), "a key only the other set held is in this one now")
  }

  test every_key_is_found_after_the_backing_table_grows() {
    mut s := Set::new()
    first := nth_key(0)
    _ := s.insert(first.as_str())
    first_cap := s.capacity()
    mut i := 1 as usize
    while i < 40 {
      k := nth_key(i)
      _ := s.insert(k.as_str())
      i = i + 1
    }
    assert(s.capacity() > first_cap, "the inserts outgrew the room the set started with")
    assert(s.len() == 40, "forty inserts are forty members")
    mut j := 0 as usize
    while j < 40 {
      k := nth_key(j)
      assert(s.contains(k.as_str()), "a key inserted before a growth is still a member")
      j = j + 1
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }

  // A distinct two-letter key per `n` below 676, so a test can
  // enumerate keys without a number formatter.
  fn nth_key(n: usize) -> String {
    mut s := String::with_capacity(2)
    s.push(0x61u8 + (n / 26) as u8)
    s.push(0x61u8 + (n % 26) as u8)
    ret s
  }
}
