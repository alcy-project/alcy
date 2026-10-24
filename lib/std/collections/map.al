// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/collections`: a hash table from `str` keys to owned values.

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

// The table's hash is the standard library's one, over the key's
// bytes; equality stays a byte walk.
fn hash(s: str) -> u64 {
  ret xxh3_64(s)
}

// Key equality through `str`'s `PartialEq`.
fn key_eq(a: str, b: str) -> bool {
  ret a == b
}

// The slot count that backs `n` inserts under the three-quarters load
// factor `insert` grows at: `ceil(4n/3)`, with the sum refused when it
// would not fit in a `usize`.
fn slots_for(n: usize) -> usize {
  // The largest `usize`, through the wrapping form so a debug build
  // does not trip the checked subtraction.
  if n > wrapping_sub(0 as usize, 1 as usize) / 4 * 3 {
    panic("capacity overflow")
  }
  ret n + (n + 2) / 3
}

impl<V> Map<V> {
  pub fn new() -> Map<V> {
    ret Map::<V>::with_capacity(0)
  }

  // Reserves room for `n` entries without changing the length. The
  // table grows at three quarters occupancy, so the slots are the
  // smallest count that keeps `n` inserts from rehashing. A zero `n`
  // still yields a distinct, freeable block, like `Vec`.
  pub fn with_capacity(n: usize) -> Map<V> {
    slots := slots_for(n)
    keys := unsafe { alloc::<String>(slots) }
    values := unsafe { alloc::<V>(slots) }
    states := unsafe { alloc::<u8>(slots) }
    mut i := 0 as usize
    while i < slots {
      unsafe { uninit_write(elem_ptr(states, i), 0 as u8) }
      i = i + 1
    }
    ret Map { keys: keys, values: values, states: states, len: 0, tombs: 0, cap: slots, }
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
      st := *uninit_ref(unsafe { elem_ref(self.states, slot) })
      if st == 0 {
        if free < self.cap {
          result = free
        } else {
          result = slot
        }
        found = true
      } else {
        if st == 1 {
          if key_eq(uninit_ref(unsafe { elem_ref(self.keys, slot) }).as_str(), key) {
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
    if *uninit_ref(unsafe { elem_ref(self.states, slot) }) != 1 {
      ret Option::None
    }
    ret Option::Some(uninit_ref(unsafe { elem_ref(self.values, slot) }))
  }

  // The value under `key` as a mutable reference, or `None`.
  pub fn get_mut(mut self: &mut Self, key: str) -> Option<&mut V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(key)
    if *uninit_ref(unsafe { elem_ref(self.states, slot) }) != 1 {
      ret Option::None
    }
    ret Option::Some(unsafe { uninit_assume(elem_ptr(self.values, slot)) })
  }

  // The value under `key`, inserting `value` first when the key is
  // absent. The value is dropped when the key is already present.
  pub fn get_or_insert(mut self: &mut Self, key: str, value: V) -> &mut V {
    if !self.contains(key) {
      _ := self.insert(key, value)
    }
    ret self.get_mut(key).or_panic_with_message("the key was just inserted")
  }

  // The value for `key`, inserting `make()`'s when absent, so the
  // default is only built on a miss.
  pub fn get_or_insert_with(mut self: &mut Self, key: str, make: () -> V) -> &mut V {
    if !self.contains(key) {
      _ := self.insert(key, make())
    }
    ret self.get_mut(key).or_panic_with_message("the key was just inserted")
  }

  // The entry for `key`: the value when present, or the map and key to
  // fill when absent.
  pub fn entry(mut self: &mut Self, key: str) -> Entry<V> {
    if self.contains(key) {
      ret Entry::Occupied(self.get_mut(key).or_panic_with_message("the key was just inserted"))
    }
    ret Entry::Vacant(self, key)
  }

  pub fn contains(self: &Self, key: str) -> bool {
    if self.cap == 0 {
      ret false
    }
    slot := self.locate(key)
    ret *uninit_ref(unsafe { elem_ref(self.states, slot) }) == 1
  }

  // Inserts `value` under `key`, replacing and returning the value
  // already there, if any. The key is copied.
  pub fn insert(mut self: &mut Self, key: str, value: V) -> Option<V> {
    if self.cap == 0 || (self.len + self.tombs + 1) * 4 > self.cap * 3 {
      self.grow()
    }
    slot := self.locate(key)
    st := *uninit_ref(unsafe { elem_ref(self.states, slot) })
    if st == 1 {
      old := unsafe { *uninit_assume(elem_ptr(self.values, slot)) }
      unsafe { uninit_write(elem_ptr(self.values, slot), value) }
      ret Option::Some(old)
    }
    mut owned := String::with_capacity(str_len(key))
    owned.push_str(key)
    unsafe { uninit_write(elem_ptr(self.keys, slot), owned) }
    unsafe { uninit_write(elem_ptr(self.values, slot), value) }
    unsafe { uninit_write(elem_ptr(self.states, slot), 1 as u8) }
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
    if *uninit_ref(unsafe { elem_ref(self.states, slot) }) != 1 {
      ret Option::None
    }
    old := unsafe { *uninit_assume(elem_ptr(self.values, slot)) }
    dead := unsafe { *uninit_assume(elem_ptr(self.keys, slot)) }
    unsafe { uninit_write(elem_ptr(self.states, slot), 2 as u8) }
    self.len = self.len - 1
    self.tombs = self.tombs + 1
    ret Option::Some(old)
  }

  // Ends every entry, keeping the room already reserved.
  pub fn clear(mut self: &mut Self) {
    mut i := 0 as usize
    while i < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, i) }) == 1 {
        dead_key := unsafe { *uninit_assume(elem_ptr(self.keys, i)) }
        dead_value := unsafe { *uninit_assume(elem_ptr(self.values, i)) }
      }
      unsafe { uninit_write(elem_ptr(self.states, i), 0 as u8) }
      i = i + 1
    }
    self.len = 0
    self.tombs = 0
  }

  // Moves to a table twice the size, or to a first table of eight,
  // rehashing the live entries into it.
  fn grow(mut self: &mut Self) {
    mut next := doubled_capacity(self.cap)
    if self.cap == 0 {
      next = 8
    }
    keys := unsafe { alloc::<String>(next) }
    values := unsafe { alloc::<V>(next) }
    states := unsafe { alloc::<u8>(next) }
    mut i := 0 as usize
    while i < next {
      unsafe { uninit_write(elem_ptr(states, i), 0 as u8) }
      i = i + 1
    }
    mut j := 0 as usize
    while j < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, j) }) == 1 {
        k := unsafe { *uninit_assume(elem_ptr(self.keys, j)) }
        v := unsafe { *uninit_assume(elem_ptr(self.values, j)) }
        mut slot := (hash(k.as_str()) as usize) % next
        while *uninit_ref(unsafe { elem_ref(states, slot) }) != 0 {
          slot = (slot + 1) % next
        }
        unsafe { uninit_write(elem_ptr(keys, slot), k) }
        unsafe { uninit_write(elem_ptr(values, slot), v) }
        unsafe { uninit_write(elem_ptr(states, slot), 1 as u8) }
      }
      j = j + 1
    }
    unsafe { dealloc(self.keys, self.cap) }
    unsafe { dealloc(self.values, self.cap) }
    unsafe { dealloc(self.states, self.cap) }
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
      if *uninit_ref(unsafe { elem_ref(self.states, i) }) == 1 {
        dead_key := unsafe { *uninit_assume(elem_ptr(self.keys, i)) }
        dead_value := unsafe { *uninit_assume(elem_ptr(self.values, i)) }
      }
      i = i + 1
    }
    unsafe { dealloc(self.keys, self.cap) }
    unsafe { dealloc(self.values, self.cap) }
    unsafe { dealloc(self.states, self.cap) }
  }

  // Shared cursors over the live entries. The borrows of the three
  // buffers are what keep an entry readable while the cursor lives.
  pub fn iter(self: &Self) -> MapEntryIter<V> {
    ret MapEntryIter {
      keys: self.keys,
      values: self.values,
      states: self.states,
      cap: self.cap,
      pos: 0,
    }
  }

  pub fn keys(self: &Self) -> MapKeys<V> {
    ret MapKeys { keys: self.keys, states: self.states, cap: self.cap, pos: 0 }
  }

  pub fn values(self: &Self) -> MapValues<V> {
    ret MapValues { values: self.values, states: self.states, cap: self.cap, pos: 0 }
  }
}

// The first live slot at or after `pos`, or `cap` when there is none.
fn next_live(states: &MaybeUninit<u8>, cap: usize, pos: usize) -> usize {
  mut at := pos
  while at < cap {
    if *uninit_ref(unsafe { elem_ref(states, at) }) == 1 {
      ret at
    }
    at = at + 1
  }
  ret cap
}

// A shared cursor over a map's entries, yielding the key as `str` and
// the value by reference.
pub struct MapEntryIter<V> {
  keys: &MaybeUninit<String>,
  values: &MaybeUninit<V>,
  states: &MaybeUninit<u8>,
  cap: usize,
  pos: usize,
}

impl<V> MapEntryIter<V> {
  pub fn into_iter(self: MapEntryIter<V>) -> MapEntryIter<V> {
    ret self
  }
}

impl<V> Iterator < (str, &V) > for MapEntryIter<V> {
  fn next(mut self: &mut Self) -> Option<(str, &V)> {
    self.pos = next_live(self.states, self.cap, self.pos)
    if self.pos >= self.cap {
      ret Option::None
    }
    k := uninit_ref(unsafe { elem_ref(self.keys, self.pos) })
    v := uninit_ref(unsafe { elem_ref(self.values, self.pos) })
    self.pos = self.pos + 1
    ret Option::Some((k.as_str(), v))
  }
}

// A shared cursor over a map's keys.
pub struct MapKeys<V> {
  keys: &MaybeUninit<String>,
  states: &MaybeUninit<u8>,
  cap: usize,
  pos: usize,
}

impl<V> MapKeys<V> {
  pub fn into_iter(self: MapKeys<V>) -> MapKeys<V> {
    ret self
  }
}

impl<V> Iterator < str > for MapKeys<V> {
  fn next(mut self: &mut Self) -> Option<str> {
    self.pos = next_live(self.states, self.cap, self.pos)
    if self.pos >= self.cap {
      ret Option::None
    }
    k := uninit_ref(unsafe { elem_ref(self.keys, self.pos) })
    self.pos = self.pos + 1
    ret Option::Some(k.as_str())
  }
}

// A shared cursor over a map's values.
pub struct MapValues<V> {
  values: &MaybeUninit<V>,
  states: &MaybeUninit<u8>,
  cap: usize,
  pos: usize,
}

impl<V> MapValues<V> {
  pub fn into_iter(self: MapValues<V>) -> MapValues<V> {
    ret self
  }
}

impl<V> Iterator < &V > for MapValues<V> {
  fn next(mut self: &mut Self) -> Option<&V> {
    self.pos = next_live(self.states, self.cap, self.pos)
    if self.pos >= self.cap {
      ret Option::None
    }
    v := uninit_ref(unsafe { elem_ref(self.values, self.pos) })
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

impl<V> Map<V> {
  // Consumes the map; the cursor owns the entries that remain.
  pub fn into_iter(self: Map<V>) -> MapIntoIter<V> {
    ret MapIntoIter { m: self, pos: 0 }
  }
}

// A consuming cursor over a map. The taken entry leaves as its key and
// value; the cursor's destructor ends the entries a loop never took
// and releases the three buffers.
pub struct MapIntoIter<V> {
  m: Map<V>,
  pos: usize,
}

impl<V> MapIntoIter<V> {
  pub fn into_iter(self: MapIntoIter<V>) -> MapIntoIter<V> {
    ret self
  }

  pub fn drop(self: MapIntoIter<V>) {
    mut i := self.pos
    while i < self.m.cap {
      if *uninit_ref(unsafe { elem_ref(self.m.states, i) }) == 1 {
        unsafe { drop_in_place::<String>(uninit_assume(elem_ptr(self.m.keys, i))) }
        unsafe { drop_in_place::<V>(uninit_assume(elem_ptr(self.m.values, i))) }
      }
      i = i + 1
    }
    unsafe { dealloc(self.m.keys, self.m.cap) }
    unsafe { dealloc(self.m.values, self.m.cap) }
    unsafe { dealloc(self.m.states, self.m.cap) }
  }
}

impl<V> Iterator < (String, V) > for MapIntoIter<V> {
  fn next(mut self: &mut Self) -> Option<(String, V)> {
    at := next_live(self.m.states, self.m.cap, self.pos)
    if at >= self.m.cap {
      ret Option::None
    }
    k := unsafe { *uninit_assume(elem_ptr(self.m.keys, at)) }
    v := unsafe { *uninit_assume(elem_ptr(self.m.values, at)) }
    self.pos = at + 1
    ret Option::Some((k, v))
  }
}

// `m[key]` reads and writes the value under `key`, panicking when the
// key is absent. The panic names the map rather than the `Option` the
// lookup hands back.
impl<V> Index < str, V > for Map<V> {
  fn index(self: &Self, i: str) -> &V {
    ret match self.get(i) {
      Option::Some(v) => v,
      Option::None => panic("Map has no entry for the key"),
    }
  }
}

impl<V> IndexMut < str, V > for Map<V> {
  fn index_mut(mut self: &mut Self, i: str) -> &mut V {
    ret match self.get_mut(i) {
      Option::Some(v) => v,
      Option::None => panic("Map has no entry for the key"),
    }
  }
}

// `==` compares the live entries: same length, and every key of one
// map present in the other with an equal value. Keys compare by the
// byte equality `locate` uses, and values through `PartialEq`.
impl<V: PartialEq> PartialEq for Map<V> {
  fn eq(self: &Self, other: &Self) -> bool {
    if self.len() != other.len() {
      ret false
    }
    mut i := 0 as usize
    while i < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, i) }) == 1 {
        key := uninit_ref(unsafe { elem_ref(self.keys, i) }).as_str()
        mine := uninit_ref(unsafe { elem_ref(self.values, i) })
        match other.get(key) {
          Option::Some(v) => {
            if *v != *mine {
              ret false
            }
          }
          Option::None => {
            ret false
          }
        }
      }
      i = i + 1
    }
    ret true
  }
}

// A place in a map's table: the value when the key is present, or the
// map and key to insert into when it is not. `or_insert` is the single
// call that fills an absent key.
pub enum Entry<V> {
  Occupied(&mut V),
  Vacant(&mut Map<V>, str),
}

impl<V> Entry<V> {
  // The value the entry names, inserting `default` first when the key
  // was absent.
  pub fn or_insert(self: Entry<V>, default: V) -> &mut V {
    match self {
      Entry::Occupied(v) => ret v,
      Entry::Vacant(map, key) => ret map.get_or_insert(key, default),
    }
  }
}
