// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: a hash table from `str` keys to owned values.

// A map keyed by `str` and hashed by its bytes. Keys are copied, so
// the table owns them: changing or ending the string an entry was
// inserted from leaves the entry alone. Values move with the table,
// and `remove`, `clear`, and the map's own destructor end the keys
// they release.
//
// The table is open addressing with linear probing. A slot is empty
// (0), full (1), or removed (2). A lookup walks past removed slots
// and stops at an empty one; a removed slot is also where the next
// missing key lands, so the table reuses its holes instead of
// rehashing on every removal. Growth rehashes at three quarters
// occupancy, counting removed slots, so insertion stays amortized.
//
// Nothing bounds `V`: the hash is this module's own and equality is a
// byte walk, so no capability has to be declared to use the map. The
// key type stays `str` until `spec` can demand a hash and equality;
// see `docs/adr/0024-str-keyed-collections-before-specs.md`.
pub struct Map<V> {
  keys: &mut MaybeUninit<String>,
  values: &mut MaybeUninit<V>,
  states: &mut MaybeUninit<u8>,
  len: usize,
  tombs: usize,
  cap: usize,
}

// FNV-1a over the bytes. The multiply wraps, which is the point.
fn hash(s: str) -> u64 {
  mut h := 2166136261 as u64
  mut i := 0 as usize
  while i < str_len(s) {
    h = (h ^ (str_byte(s, i) as u64)) * 0x100000001b3 as u64
    i = i + 1
  }
  ret h
}

// Byte equality, because `str` has no comparison yet.
fn key_eq(a: str, b: str) -> bool {
  if str_len(a) != str_len(b) {
    ret false
  }
  mut i := 0 as usize
  while i < str_len(a) {
    if str_byte(a, i) != str_byte(b, i) {
      ret false
    }
    i = i + 1
  }
  ret true
}

impl<V> Map<V> {
  pub fn new() -> Map<V> {
    ret Map::<V>::with_capacity(0)
  }

  // Reserves room for `n` entries without changing the length. A zero
  // `n` still yields a distinct, freeable block, like `Vec`.
  pub fn with_capacity(n: usize) -> Map<V> {
    keys := alloc::<String>(n)
    values := alloc::<V>(n)
    states := alloc::<u8>(n)
    mut i := 0 as usize
    while i < n {
      uninit_write(elem_ptr(states, i), 0 as u8)
      i = i + 1
    }
    ret Map {
      keys: keys,
      values: values,
      states: states,
      len: 0,
      tombs: 0,
      cap: n,
    }
  }

  pub fn len(self: &Self) -> usize {
    ret self.len
  }

  pub fn is_empty(self: &Self) -> bool {
    ret self.len == 0
  }

  pub fn capacity(self: &Self) -> usize {
    ret self.cap
  }

  // The slot holding `key`, or the slot a missing key would be
  // written to: the first removed slot on the walk, or the empty slot
  // that ends it. Only called on a table with room.
  fn locate(self: &Self, key: str) -> usize {
    mut slot := (hash(key) as usize) % self.cap
    mut free := self.cap
    mut result := slot
    mut found := false
    while !found {
      st := *uninit_ref(elem_ref(self.states, slot))
      if st == 0 {
        if free < self.cap {
          result = free
        } else {
          result = slot
        }
        found = true
      } else {
        if st == 1 {
          if key_eq(uninit_ref(elem_ref(self.keys, slot)).as_str(), key) {
            result = slot
            found = true
          }
        } else {
          if free == self.cap {
            free = slot
          }
        }
        if !found {
          slot = (slot + 1) % self.cap
        }
      }
    }
    ret result
  }

  // The value under `key`, or `None`. The result borrows the map, so
  // the entry stays put while it is read.
  pub fn get(self: &Self, key: str) -> Option<&V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(key)
    if *uninit_ref(elem_ref(self.states, slot)) != 1 {
      ret Option::None
    }
    ret Option::Some(uninit_ref(elem_ref(self.values, slot)))
  }

  // The value under `key` as a mutable reference, or `None`.
  pub fn get_mut(mut self: &mut Self, key: str) -> Option<&mut V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(key)
    if *uninit_ref(elem_ref(self.states, slot)) != 1 {
      ret Option::None
    }
    ret Option::Some(uninit_assume(elem_ptr(self.values, slot)))
  }

  pub fn contains(self: &Self, key: str) -> bool {
    if self.cap == 0 {
      ret false
    }
    slot := self.locate(key)
    ret *uninit_ref(elem_ref(self.states, slot)) == 1
  }

  // Inserts `value` under `key`, replacing and returning the value
  // already there, if any. The key is copied.
  pub fn insert(mut self: &mut Self, key: str, value: V) -> Option<V> {
    if self.cap == 0 || (self.len + self.tombs + 1) * 4 > self.cap * 3 {
      self.grow()
    }
    slot := self.locate(key)
    st := *uninit_ref(elem_ref(self.states, slot))
    if st == 1 {
      old := *uninit_assume(elem_ptr(self.values, slot))
      uninit_write(elem_ptr(self.values, slot), value)
      ret Option::Some(old)
    }
    mut owned := String::with_capacity(str_len(key))
    owned.push_str(key)
    uninit_write(elem_ptr(self.keys, slot), owned)
    uninit_write(elem_ptr(self.values, slot), value)
    uninit_write(elem_ptr(self.states, slot), 1 as u8)
    self.len = self.len + 1
    if st == 2 {
      self.tombs = self.tombs - 1
    }
    ret Option::None
  }

  // Removes the entry and returns its value; the key is ended.
  pub fn remove(mut self: &mut Self, key: str) -> Option<V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(key)
    if *uninit_ref(elem_ref(self.states, slot)) != 1 {
      ret Option::None
    }
    old := *uninit_assume(elem_ptr(self.values, slot))
    dead := *uninit_assume(elem_ptr(self.keys, slot))
    uninit_write(elem_ptr(self.states, slot), 2 as u8)
    self.len = self.len - 1
    self.tombs = self.tombs + 1
    ret Option::Some(old)
  }

  // Ends every entry, keeping the room already reserved.
  pub fn clear(mut self: &mut Self) {
    mut i := 0 as usize
    while i < self.cap {
      if *uninit_ref(elem_ref(self.states, i)) == 1 {
        dead_key := *uninit_assume(elem_ptr(self.keys, i))
        dead_value := *uninit_assume(elem_ptr(self.values, i))
      }
      uninit_write(elem_ptr(self.states, i), 0 as u8)
      i = i + 1
    }
    self.len = 0
    self.tombs = 0
  }

  // Moves to a table twice the size, or to a first table of eight,
  // rehashing the live entries into it.
  fn grow(mut self: &mut Self) {
    mut next := self.cap * 2
    if self.cap == 0 {
      next = 8
    }
    keys := alloc::<String>(next)
    values := alloc::<V>(next)
    states := alloc::<u8>(next)
    mut i := 0 as usize
    while i < next {
      uninit_write(elem_ptr(states, i), 0 as u8)
      i = i + 1
    }
    mut j := 0 as usize
    while j < self.cap {
      if *uninit_ref(elem_ref(self.states, j)) == 1 {
        k := *uninit_assume(elem_ptr(self.keys, j))
        v := *uninit_assume(elem_ptr(self.values, j))
        mut slot := (hash(k.as_str()) as usize) % next
        while *uninit_ref(elem_ref(states, slot)) != 0 {
          slot = (slot + 1) % next
        }
        uninit_write(elem_ptr(keys, slot), k)
        uninit_write(elem_ptr(values, slot), v)
        uninit_write(elem_ptr(states, slot), 1 as u8)
      }
      j = j + 1
    }
    dealloc(self.keys, self.cap)
    dealloc(self.values, self.cap)
    dealloc(self.states, self.cap)
    self.keys = keys
    self.values = values
    self.states = states
    self.cap = next
    self.tombs = 0
  }

  // Ends every entry and releases the table.
  pub fn drop(self: Map<V>) {
    mut i := 0 as usize
    while i < self.cap {
      if *uninit_ref(elem_ref(self.states, i)) == 1 {
        dead_key := *uninit_assume(elem_ptr(self.keys, i))
        dead_value := *uninit_assume(elem_ptr(self.values, i))
      }
      i = i + 1
    }
    dealloc(self.keys, self.cap)
    dealloc(self.values, self.cap)
    dealloc(self.states, self.cap)
  }
}
