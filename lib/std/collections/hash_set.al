// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/collections`: a set of `T: Hash + Eq` keys.

// A set of keys backed by `HashMap<T, u8>`. Keys move in and are owned
// by the table, so the set ends them when they are removed or when the
// set itself ends. `insert` reports whether the key was new. See
// `hash_map.al` for the table.
pub struct HashSet<T> {
  map: HashMap < T,
  u8 >,
}

impl<T: Hash + Eq> HashSet<T> {
  pub fn new() -> HashSet<T> {
    ret HashSet { map: HashMap::< T, u8 > ::new() }
  }

  // Reserves room for `n` keys without changing the length.
  pub fn with_capacity(n: usize) -> HashSet<T> {
    ret HashSet { map: HashMap::< T, u8 > ::with_capacity(n) }
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
  pub fn insert(mut self: &mut Self, key: T) -> bool {
    ret self.map.insert(key, 0 as u8).is_none()
  }

  // Moves every key of `other` in, ending `other`.
  pub fn extend(mut self: &mut Self, other: HashSet<T>) {
    mut keys := keys_of(other)
    loop {
      match keys.next() {
        Option::Some(key) => {
          _ := self.insert(key)
        }
        Option::None => {
          break
        }
      }
    }
  }

  // Whether every key of this set is also in `other`; the empty set
  // is a subset of any.
  pub fn is_subset(self: &Self, other: &Self) -> bool {
    mut cursor := self.iter()
    loop {
      match cursor.next() {
        Option::Some(key) => {
          if !other.map.contains_ref(key) {
            ret false
          }
        }
        Option::None => {
          ret true
        }
      }
    }
  }

  pub fn contains(self: &Self, key: T) -> bool {
    ret self.map.contains(key)
  }

  // Removes `key`; true when it was a member.
  pub fn remove(mut self: &mut Self, key: T) -> bool {
    ret self.map.remove(key).is_some()
  }

  pub fn clear(mut self: &mut Self) {
    self.map.clear()
  }

  // A new set with every key of either. Both inputs are consumed, so
  // non-`Copy` keys move rather than being duplicated.
  pub fn union_with(mut self: HashSet<T>, other: HashSet<T>) -> HashSet<T> {
    mut out := HashSet::<T>::with_capacity(self.len() + other.len())
    mut left := keys_of(self)
    loop {
      match left.next() {
        Option::Some(key) => {
          _ := out.insert(key)
        }
        Option::None => {
          break
        }
      }
    }
    mut right := keys_of(other)
    loop {
      match right.next() {
        Option::Some(key) => {
          _ := out.insert(key)
        }
        Option::None => {
          break
        }
      }
    }
    ret out
  }

  // A new set with only the keys both hold. Both inputs are consumed.
  pub fn intersection(self: HashSet<T>, other: HashSet<T>) -> HashSet<T> {
    mut out := HashSet::<T>::with_capacity(self.len())
    mut keys := keys_of(self)
    loop {
      match keys.next() {
        Option::Some(key) => {
          if other.map.contains_ref(&key) {
            _ := out.insert(key)
          }
        }
        Option::None => {
          break
        }
      }
    }
    ret out
  }

  // Keys end when the set does; `iter` hands them out while it lives.
  pub fn iter(self: &Self) -> HashMapKeys<T, u8> {
    ret self.map.keys()
  }
}

// Consumes the set and yields its keys, moving them out of the table.
pub fn keys_of<T>(set: HashSet<T>) -> HashSetIntoIter<T> {
  ret HashSetIntoIter { set: set, pos: 0 }
}

// A consuming cursor over a set's keys. It owns the whole set, so no
// field is moved out from under the set's destructor.
pub struct HashSetIntoIter<T> {
  set: HashSet<T>,
  pos: usize,
}

impl<T> HashSetIntoIter<T> {
  pub fn next(mut self: &mut Self) -> Option<T> {
    match self.set.map.take_key(self.pos) {
      Option::Some((next, key)) => {
        self.pos = next
        ret Option::Some(key)
      }
      Option::None => {
        ret Option::None
      }
    }
  }
}

// The backing map's equality is the set's: same length and every key of
// one present in the other.
impl<T: Hash + Eq> PartialEq for HashSet<T> {
  fn eq(self: &Self, other: &Self) -> bool {
    ret self.map == other.map
  }
}

impl<T: Hash + Eq> Eq for HashSet<T> {
}

// Tests for the set: one entry per property `HashSet` promises.
#[when(testing)] {
  #[test] fn new_set_is_empty() {
    s := HashSet::<i32>::new()
    assert(s.is_empty(), "a set that was never inserted into holds nothing")
    assert(s.len() == 0, "a set that was never inserted into is of length zero")
    assert(!s.contains(0), "an empty set has no key to find")
  }

  #[test] fn insert_of_a_new_key_reports_it() {
    mut s := HashSet::<i32>::new()
    assert(s.insert(0), "the key was not a member before")
    assert(s.len() == 1, "the insert added one member")
    assert(s.contains(0), "the member is there afterwards")
  }

  #[test] fn duplicate_insert_reports_false_and_adds_nothing() {
    mut s := HashSet::<i32>::new()
    _ := s.insert(0)
    assert(!s.insert(0), "a key inserted twice is still in the set")
    assert(s.len() == 1, "the duplicate adds no member")
  }

  #[test] fn contains_holds_only_the_inserted_keys() {
    mut s := HashSet::<i32>::new()
    _ := s.insert(0)
    _ := s.insert(1)
    assert(s.contains(1), "an inserted key is a member")
    assert(!s.contains(9), "a key never inserted is not a member")
    assert(s.len() == 2, "two inserts are two members")
  }

  #[test] fn remove_of_a_present_key_reports_true() {
    mut s := HashSet::<i32>::new()
    _ := s.insert(0)
    _ := s.insert(1)
    assert(s.remove(0), "the key was a member")
    assert(!s.contains(0), "the removed key is gone")
    assert(s.contains(1), "the other member is untouched")
    assert(s.len() == 1, "the removal shortens the set")
  }

  #[test] fn remove_of_an_absent_key_reports_false() {
    mut s := HashSet::<i32>::new()
    _ := s.insert(0)
    assert(!s.remove(9), "a key never inserted cannot be removed")
    assert(s.len() == 1, "the failed removal keeps the length")
  }

  #[test] fn clear_drops_every_member() {
    mut s := HashSet::<i32>::with_capacity(8)
    _ := s.insert(0)
    _ := s.insert(1)
    reserved := s.capacity()
    s.clear()
    assert(s.is_empty(), "clearing leaves no member")
    assert(s.capacity() == reserved, "the room reserved before the clear is kept")
    assert(!s.contains(0), "a cleared key is not a member")
    _ := s.insert(2)
    assert(s.contains(2), "the cleared set takes a new key")
  }

  #[test] fn union_holds_every_key_of_either_set() {
    (a, b) := pair()
    u := a.union_with(b)
    assert(u.len() == 3, "the shared key is one member of the union")
    assert(u.contains(0), "a key of the left set is in the union")
    assert(u.contains(2), "a key of the right set is in the union")
    assert(!u.contains(3), "a key neither set holds is not in the union")
  }

  #[test] fn intersection_holds_only_the_shared_keys() {
    (a, b) := pair()
    i := a.intersection(b)
    assert(i.len() == 1, "one key is in both sets")
    assert(i.contains(1), "the shared key is the one in the intersection")
    assert(!i.contains(0), "a key only the left set holds is not in the intersection")
  }

  #[test] fn set_is_a_subset_of_the_one_that_holds_more() {
    mut small := HashSet::<i32>::new()
    _ := small.insert(0)
    mut large := HashSet::<i32>::new()
    _ := large.insert(0)
    _ := large.insert(1)
    assert(small.is_subset(&large), "every key of the smaller set is in the larger")
    assert(!large.is_subset(&small), "the larger set holds a key the smaller one does not")
    assert(small.is_subset(&small), "a set is a subset of itself")
  }

  #[test] fn empty_set_is_a_subset_of_any_set() {
    empty := HashSet::<i32>::new()
    mut s := HashSet::<i32>::new()
    _ := s.insert(0)
    assert(empty.is_subset(&s), "a set with no keys is a subset of one with keys")
    assert(empty.is_subset(&empty), "a set with no keys is a subset of itself")
  }

  #[test] fn sets_with_the_same_keys_are_equal() {
    mut a := HashSet::<i32>::new()
    _ := a.insert(0)
    _ := a.insert(1)
    mut b := HashSet::<i32>::new()
    _ := b.insert(1)
    _ := b.insert(0)
    assert(a == b, "the same keys make equal sets whatever order they were inserted in")
    _ := b.insert(2)
    assert(!(a == b), "an extra key makes the sets differ")
  }

  #[test] fn extend_adds_every_key_of_the_other_set() {
    mut a := HashSet::<i32>::new()
    _ := a.insert(0)
    mut b := HashSet::<i32>::new()
    _ := b.insert(0)
    _ := b.insert(1)
    a.extend(b)
    assert(a.len() == 2, "the shared key is one member")
    assert(a.contains(1), "a key only the other set held is in this one now")
  }

  #[test] fn string_keys_are_members_too() {
    mut s := HashSet::<String>::new()
    _ := s.insert(String::from("alpha"))
    _ := s.insert(String::from("beta"))
    assert(s.contains(String::from("alpha")), "a string key is a member by its bytes")
    assert(!s.contains(String::from("gamma")), "a different string is not")
    assert(s.remove(String::from("alpha")), "the string key can be removed")
    assert(s.len() == 1, "one member remains")
  }

  #[test] fn every_key_is_found_after_the_backing_table_grows() {
    mut s := HashSet::<i32>::new()
    _ := s.insert(0)
    first_cap := s.capacity()
    mut i := 1
    while i < 40 {
      _ := s.insert(i)
      i = i + 1
    }
    assert(s.capacity() > first_cap, "the inserts outgrew the room the set started with")
    assert(s.len() == 40, "forty inserts are forty members")
    mut j := 0
    while j < 40 {
      assert(s.contains(j), "a key inserted before a growth is still a member")
      j = j + 1
    }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }

  fn pair() -> (HashSet<i32>, HashSet<i32>) {
    mut a := HashSet::<i32>::new()
    _ := a.insert(0)
    _ := a.insert(1)
    mut b := HashSet::<i32>::new()
    _ := b.insert(1)
    _ := b.insert(2)
    ret (a, b)
  }
}
