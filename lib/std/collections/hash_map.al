// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/collections`: a hash table from `K` keys to owned values.

// A map keyed by `K: Hash + Eq`: the digest picks a slot and equality
// resolves a probe chain.
// Keys move into the table, so the table owns them: changing or ending
// the value an entry was inserted from leaves the entry alone. Values
// move with the table, and `remove`, `clear`, and the map's own
// destructor end the keys and values they release.
//
// The table is open addressing with linear probing. A slot is empty
// (0), full (1), or removed (2). A lookup walks past removed slots and
// stops at an empty one; a removed slot is also where the next missing
// key lands, so the table reuses its holes instead of rehashing on
// every removal. Growth rehashes at three quarters occupancy, counting
// removed slots, so insertion stays amortized.
//
// Nothing bounds `V`: only the key needs a digest and equality, so a
// caller chooses what the map holds without declaring a capability.
pub struct HashMap<K, V> {
  keys: &mut MaybeUninit<K>,
  values: &mut MaybeUninit<V>,
  states: &mut MaybeUninit<u8>,
  len: usize,
  tombs: usize,
  cap: usize,
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

impl<K: Hash + Eq, V> HashMap<K, V> {
  pub fn new() -> HashMap<K, V> {
    ret HashMap::<K, V>::with_capacity(0)
  }

  // Reserves room for `n` entries without changing the length. The
  // table grows at three quarters occupancy, so the slots are the
  // smallest count that keeps `n` inserts from rehashing. A zero `n`
  // still yields a distinct, freeable block, like `Vec`.
  pub fn with_capacity(n: usize) -> HashMap<K, V> {
    slots := slots_for(n)
    keys := unsafe { alloc::<K>(slots) }
    values := unsafe { alloc::<V>(slots) }
    states := unsafe { alloc::<u8>(slots) }
    mut i := 0 as usize
    while i < slots {
      unsafe { uninit_write(elem_ptr(states, i), 0 as u8) }
      i = i + 1
    }
    ret HashMap { keys: keys, values: values, states: states, len: 0, tombs: 0, cap: slots, }
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
  fn locate(self: &Self, key: &K) -> usize {
    mut slot := (key.hash() as usize) % self.cap
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
          stored := uninit_ref(unsafe { elem_ref(self.keys, slot) })
          if *stored == *key {
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

  // Borrowed lookup, for callers that hold only `&K`.
  pub fn get_at(self: &Self, key: &K) -> Option<&V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(key)
    if *uninit_ref(unsafe { elem_ref(self.states, slot) }) != 1 {
      ret Option::None
    }
    ret Option::Some(uninit_ref(unsafe { elem_ref(self.values, slot) }))
  }

  // The value under `key`, or `None`. The result borrows the map, so
  // the entry stays put while it is read.
  pub fn get(self: &Self, key: K) -> Option<&V> {
    ret self.get_at(&key)
  }

  // The value under `key` as a mutable reference, or `None`.
  pub fn get_mut(mut self: &mut Self, key: K) -> Option<&mut V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(&key)
    if *uninit_ref(unsafe { elem_ref(self.states, slot) }) != 1 {
      ret Option::None
    }
    ret Option::Some(unsafe { uninit_assume(elem_ptr(self.values, slot)) })
  }

  // The value under `key`, inserting `value` first when the key is
  // absent. The incoming key and value end here on a hit; on a miss
  // the key moves into the new entry.
  pub fn get_or_insert(mut self: &mut Self, key: K, value: V) -> &mut V {
    if self.cap == 0 || (self.len + self.tombs + 1) * 4 > self.cap * 3 {
      self.grow()
    }
    slot := self.locate(&key)
    st := *uninit_ref(unsafe { elem_ref(self.states, slot) })
    if st == 1 {
      dead_key := key
      dead_value := value
    } else {
      unsafe { uninit_write(elem_ptr(self.keys, slot), key) }
      unsafe { uninit_write(elem_ptr(self.values, slot), value) }
      unsafe { uninit_write(elem_ptr(self.states, slot), 1 as u8) }
      self.len = self.len + 1
      if st == 2 {
        self.tombs = self.tombs - 1
      }
    }
    ret unsafe { uninit_assume(elem_ptr(self.values, slot)) }
  }

  // The value for `key`, inserting `make()`'s when absent, so the
  // default is only built on a miss.
  pub fn get_or_insert_with(mut self: &mut Self, key: K, make: () -> V) -> &mut V {
    if self.cap == 0 || (self.len + self.tombs + 1) * 4 > self.cap * 3 {
      self.grow()
    }
    slot := self.locate(&key)
    st := *uninit_ref(unsafe { elem_ref(self.states, slot) })
    if st == 1 {
      dead_key := key
    } else {
      value := make()
      unsafe { uninit_write(elem_ptr(self.keys, slot), key) }
      unsafe { uninit_write(elem_ptr(self.values, slot), value) }
      unsafe { uninit_write(elem_ptr(self.states, slot), 1 as u8) }
      self.len = self.len + 1
      if st == 2 {
        self.tombs = self.tombs - 1
      }
    }
    ret unsafe { uninit_assume(elem_ptr(self.values, slot)) }
  }

  // The entry for `key`: the value when present, or the map and key to
  // fill when absent.
  pub fn entry(mut self: &mut Self, key: K) -> Entry<K, V> {
    if self.cap != 0 {
      slot := self.locate(&key)
      if *uninit_ref(unsafe { elem_ref(self.states, slot) }) == 1 {
        dead_key := key
        ret Entry::Occupied(unsafe { uninit_assume(elem_ptr(self.values, slot)) })
      }
    }
    ret Entry::Vacant(self, key)
  }

  // Borrowed membership check, for callers that hold only `&K`.
  pub fn contains_ref(self: &Self, key: &K) -> bool {
    if self.cap == 0 {
      ret false
    }
    slot := self.locate(key)
    ret *uninit_ref(unsafe { elem_ref(self.states, slot) }) == 1
  }

  pub fn contains(self: &Self, key: K) -> bool {
    ret self.contains_ref(&key)
  }

  // Inserts `value` under `key`, replacing and returning the value
  // already there, if any. On a hit the stored key stays and the
  // incoming one ends here.
  pub fn insert(mut self: &mut Self, key: K, value: V) -> Option<V> {
    if self.cap == 0 || (self.len + self.tombs + 1) * 4 > self.cap * 3 {
      self.grow()
    }
    slot := self.locate(&key)
    st := *uninit_ref(unsafe { elem_ref(self.states, slot) })
    if st == 1 {
      old := unsafe { *uninit_assume(elem_ptr(self.values, slot)) }
      dead_key := key
      unsafe { uninit_write(elem_ptr(self.values, slot), value) }
      ret Option::Some(old)
    }
    unsafe { uninit_write(elem_ptr(self.keys, slot), key) }
    unsafe { uninit_write(elem_ptr(self.values, slot), value) }
    unsafe { uninit_write(elem_ptr(self.states, slot), 1 as u8) }
    self.len = self.len + 1
    if st == 2 {
      self.tombs = self.tombs - 1
    }
    ret Option::None
  }

  // Removes the entry and returns its value; the key is ended.
  pub fn remove(mut self: &mut Self, key: K) -> Option<V> {
    if self.cap == 0 {
      ret Option::None
    }
    slot := self.locate(&key)
    if *uninit_ref(unsafe { elem_ref(self.states, slot) }) != 1 {
      ret Option::None
    }
    old := unsafe { *uninit_assume(elem_ptr(self.values, slot)) }
    dead_key := unsafe { *uninit_assume(elem_ptr(self.keys, slot)) }
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
    keys := unsafe { alloc::<K>(next) }
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
        mut slot := (k.hash() as usize) % next
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

  // Shared cursors over the live entries. The borrows of the three
  // buffers are what keep an entry readable while the cursor lives.
  pub fn iter(self: &Self) -> HashMapEntryIter<K, V> {
    ret HashMapEntryIter {
      keys: self.keys,
      values: self.values,
      states: self.states,
      cap: self.cap,
      pos: 0,
    }
  }

  pub fn keys(self: &Self) -> HashMapKeys<K, V> {
    ret HashMapKeys { keys: self.keys, states: self.states, cap: self.cap, pos: 0 }
  }

  pub fn values(self: &Self) -> HashMapValues<K, V> {
    ret HashMapValues { values: self.values, states: self.states, cap: self.cap, pos: 0 }
  }
}

impl<K, V> HashMap<K, V> {
  // Iteration primitive for `HashSetIntoIter`: takes the first live
  // key at or after `pos` out of the table and returns it with the
  // next position to scan. The slot is marked empty so the map's
  // destructor will not end the key again, but `len` is left stale;
  // callers must be consuming the map, not reading `len` afterwards.
  pub fn take_key(mut self: &mut Self, pos: usize) -> Option<(usize, K)> {
    mut at := pos
    while at < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, at) }) == 1 {
        k := unsafe { *uninit_assume(elem_ptr(self.keys, at)) }
        unsafe { uninit_write(elem_ptr(self.states, at), 0 as u8) }
        ret Option::Some((at + 1, k))
      }
      at = at + 1
    }
    ret Option::None
  }

  // Ends every entry and releases the table.
  pub fn drop(self: HashMap < K, V >) {
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
}

fn doubled_capacity(cap: usize) -> usize {
  ret cap * 2
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

// A shared cursor over a map's entries, yielding the key and value by
// reference.
pub struct HashMapEntryIter<K, V> {
  keys: &MaybeUninit<K>,
  values: &MaybeUninit<V>,
  states: &MaybeUninit<u8>,
  cap: usize,
  pos: usize,
}

impl<K, V> HashMapEntryIter<K, V> {
  pub fn into_iter(self: HashMapEntryIter < K, V >) -> HashMapEntryIter<K, V> {
    ret self
  }
}

impl<K, V> Iterator < (&K, &V) > for HashMapEntryIter<K, V> {
  fn next(mut self: &mut Self) -> Option<(&K, &V)> {
    self.pos = next_live(self.states, self.cap, self.pos)
    if self.pos >= self.cap {
      ret Option::None
    }
    k := uninit_ref(unsafe { elem_ref(self.keys, self.pos) })
    v := uninit_ref(unsafe { elem_ref(self.values, self.pos) })
    self.pos = self.pos + 1
    ret Option::Some((k, v))
  }
}

// A shared cursor over a map's keys.
pub struct HashMapKeys<K, V> {
  keys: &MaybeUninit<K>,
  states: &MaybeUninit<u8>,
  cap: usize,
  pos: usize,
}

impl<K, V> HashMapKeys<K, V> {
  pub fn into_iter(self: HashMapKeys < K, V >) -> HashMapKeys<K, V> {
    ret self
  }
}

impl<K, V> Iterator < &K > for HashMapKeys<K, V> {
  fn next(mut self: &mut Self) -> Option<&K> {
    self.pos = next_live(self.states, self.cap, self.pos)
    if self.pos >= self.cap {
      ret Option::None
    }
    k := uninit_ref(unsafe { elem_ref(self.keys, self.pos) })
    self.pos = self.pos + 1
    ret Option::Some(k)
  }
}

// A shared cursor over a map's values.
pub struct HashMapValues<K, V> {
  values: &MaybeUninit<V>,
  states: &MaybeUninit<u8>,
  cap: usize,
  pos: usize,
}

impl<K, V> HashMapValues<K, V> {
  pub fn into_iter(self: HashMapValues < K, V >) -> HashMapValues<K, V> {
    ret self
  }
}

impl<K, V> Iterator < &V > for HashMapValues<K, V> {
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

// Consumes the map; the cursor owns the entries that remain.
impl<K, V> HashMap<K, V> {
  pub fn into_iter(self: HashMap < K, V >) -> HashMapIntoIter<K, V> {
    ret into_iter_of::<K, V>(self)
  }
}

// Free constructor so generic code in another module can take a
// consuming cursor without method lookup on a generic receiver.
pub fn into_iter_of<K, V>(m: HashMap < K, V >) -> HashMapIntoIter<K, V> {
  ret HashMapIntoIter { m: m, pos: 0 }
}

// A consuming cursor over a map. The taken entry leaves as its key and
// value; the cursor's destructor ends the entries a loop never took
// and releases the three buffers.
pub struct HashMapIntoIter<K, V> {
  m: HashMap < K,
  V >,
  pos: usize,
}

impl<K, V> HashMapIntoIter<K, V> {
  pub fn into_iter(self: HashMapIntoIter < K, V >) -> HashMapIntoIter<K, V> {
    ret self
  }

  pub fn drop(self: HashMapIntoIter < K, V >) {
    mut i := self.pos
    while i < self.m.cap {
      if *uninit_ref(unsafe { elem_ref(self.m.states, i) }) == 1 {
        unsafe { drop_in_place::<K>(uninit_assume(elem_ptr(self.m.keys, i))) }
        unsafe { drop_in_place::<V>(uninit_assume(elem_ptr(self.m.values, i))) }
      }
      i = i + 1
    }
    unsafe { dealloc(self.m.keys, self.m.cap) }
    unsafe { dealloc(self.m.values, self.m.cap) }
    unsafe { dealloc(self.m.states, self.m.cap) }
  }
}

impl<K, V> Iterator < (K, V) > for HashMapIntoIter<K, V> {
  fn next(mut self: &mut Self) -> Option<(K, V)> {
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
// key is absent. The lookup scans the table by equality; the hash is
// used by the map's own methods, which find the same slot faster.
// Prefer `get` and `get_mut` for a hashed lookup, especially in a loop.
impl<K: Eq, V> Index < K, V > for HashMap<K, V> {
  fn index(self: &Self, i: K) -> &V {
    mut slot := 0 as usize
    while slot < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, slot) }) == 1 {
        stored := uninit_ref(unsafe { elem_ref(self.keys, slot) })
        if *stored == i {
          ret uninit_ref(unsafe { elem_ref(self.values, slot) })
        }
      }
      slot = slot + 1
    }
    panic("HashMap has no entry for the key")
  }
}

impl<K: Eq, V> IndexMut < K, V > for HashMap<K, V> {
  fn index_mut(mut self: &mut Self, i: K) -> &mut V {
    mut slot := 0 as usize
    while slot < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, slot) }) == 1 {
        stored := uninit_ref(unsafe { elem_ref(self.keys, slot) })
        if *stored == i {
          ret unsafe { uninit_assume(elem_ptr(self.values, slot)) }
        }
      }
      slot = slot + 1
    }
    panic("HashMap has no entry for the key")
  }
}

// `==` compares the live entries: same length, and every key of one
// map present in the other with an equal value. Keys compare through
// `Eq` and values through `PartialEq`.
impl<K: Hash + Eq, V: PartialEq> PartialEq for HashMap<K, V> {
  fn eq(self: &Self, other: &Self) -> bool {
    if self.len() != other.len() {
      ret false
    }
    mut i := 0 as usize
    while i < self.cap {
      if *uninit_ref(unsafe { elem_ref(self.states, i) }) == 1 {
        key := uninit_ref(unsafe { elem_ref(self.keys, i) })
        mine := uninit_ref(unsafe { elem_ref(self.values, i) })
        mut found := false
        mut j := 0 as usize
        while j < other.cap && !found {
          if *uninit_ref(unsafe { elem_ref(other.states, j) }) == 1 {
            other_key := uninit_ref(unsafe { elem_ref(other.keys, j) })
            if *key == *other_key {
              other_value := uninit_ref(unsafe { elem_ref(other.values, j) })
              if *other_value != *mine {
                ret false
              }
              found = true
            }
          }
          j = j + 1
        }
        if !found {
          ret false
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
pub enum Entry<K, V> {
  Occupied(&mut V),
  Vacant(&mut HashMap < K, V >, K),
}

impl<K: Hash + Eq, V> Entry<K, V> {
  // The value the entry names, inserting `default` first when the key
  // was absent.
  pub fn or_insert(self: Entry < K, V >, default: V) -> &mut V {
    match self {
      Entry::Occupied(v) => ret v,
      Entry::Vacant(map, key) => ret map.get_or_insert(key, default),
    }
  }
}

// Tests for the table: one entry per property `HashMap` promises.
#[when(testing)] {
  #[test] fn new_map_is_empty() {
    m := HashMap::<i32, i32>::new()
    assert(m.is_empty(), "a map that was never inserted into holds nothing")
    assert(m.len() == 0, "a map that was never inserted into is of length zero")
    assert(m.get(0).is_none(), "an empty table has no key to find")
  }

  #[test] fn insert_of_a_new_key_then_get_returns_its_value() {
    mut m := HashMap::<i32, i32>::new()
    first := m.insert(0, 1)
    assert(first.is_none(), "the key was not there before")
    assert(m.len() == 1, "the insert added one entry")
    assert(value_at(&m, 0) == 1, "the entry holds the value it was given")
  }

  #[test] fn inserting_an_existing_key_replaces_the_value() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    replaced := m.insert(0, 2)
    assert(
      replaced.or_panic("the key was there") == 1,
      "the insert hands back what it replaced",
    )
    assert(m.len() == 1, "replacing a key does not add an entry")
    assert(value_at(&m, 0) == 2, "the entry holds the newer value")
  }

  #[test] fn get_of_an_absent_key_is_none() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    assert(m.get(9).is_none(), "the table holds a different key")
    assert(m.len() == 1, "a failed lookup changes nothing")
  }

  #[test] fn remove_of_a_present_key_returns_its_value() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    _ := m.insert(1, 2)
    gone := m.remove(0)
    assert(
      gone.or_panic("the key was there") == 1,
      "the removed entry's value comes back",
    )
    assert(m.len() == 1, "the removal shortens the map")
    assert(m.get(0).is_none(), "the removed key reads as absent")
    assert(value_at(&m, 1) == 2, "the other entry is untouched")
  }

  #[test] fn remove_of_an_absent_key_reports_nothing() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    assert(m.remove(9).is_none(), "a key the map does not hold comes back empty-handed")
    assert(m.len() == 1, "the failed removal keeps the length")
  }

  #[test] fn clear_drops_every_entry_and_keeps_the_room() {
    mut m := HashMap::<i32, i32>::with_capacity(8)
    _ := m.insert(0, 1)
    _ := m.insert(1, 2)
    reserved := m.capacity()
    m.clear()
    assert(m.is_empty(), "clearing leaves no entry")
    assert(m.capacity() == reserved, "the room reserved before the clear is kept")
    assert(m.get(0).is_none(), "a cleared key reads as absent")
    _ := m.insert(2, 3)
    assert(value_at(&m, 2) == 3, "the cleared table takes a new key")
  }

  #[test] fn owned_string_keys_are_hashed_and_compared() {
    mut m := HashMap::<String, i32>::new()
    _ := m.insert(String::from("alpha"), 1)
    _ := m.insert(String::from("beta"), 2)
    assert(value_at_str(&m, String::from("alpha")) == 1, "a string key is found by its bytes")
    assert(m.get(String::from("gamma")).is_none(), "a different string is absent")
    assert(m.len() == 2, "two distinct strings are two entries")
  }

  #[test] fn every_key_is_found_after_the_table_grows() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 0)
    first_cap := m.capacity()
    mut i := 1
    while i < 40 {
      _ := m.insert(i, i)
      i = i + 1
    }
    assert(m.capacity() > first_cap, "the inserts outgrew the room the table started with")
    assert(m.len() == 40, "forty inserts are forty entries")
    mut j := 0
    while j < 40 {
      assert(value_at(&m, j) == j, "a key inserted before a growth is still found")
      j = j + 1
    }
  }

  #[test] fn iteration_visits_every_entry_once() {
    mut m := HashMap::<i32, i32>::new()
    mut i := 0
    while i < 40 {
      _ := m.insert(i, i)
      i = i + 1
    }
    mut seen := 0
    mut total := 0
    for (_, v) in m.iter() {
      total = total + *v
      seen = seen + 1
    }
    assert(seen == 40, "the cursor yields one item per entry")
    assert(total == 780, "each value is yielded exactly once")
  }

  #[test] fn into_iter_yields_every_entry() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    _ := m.insert(1, 2)
    mut seen := 0
    mut total := 0
    for (_, v) in m.into_iter() {
      seen = seen + 1
      total = total + v
    }
    assert(seen == 2, "the consuming cursor yields one item per entry")
    assert(total == 3, "the values come back with their keys")
  }

  #[test] fn key_inserted_after_a_remove_is_found() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    _ := m.insert(1, 2)
    _ := m.remove(0)
    _ := m.insert(2, 3)
    assert(value_at(&m, 2) == 3, "the slot a removal freed takes a new key")
    assert(value_at(&m, 1) == 2, "a key past the freed slot is still found")
    assert(m.len() == 2, "one removal and one insert cancel out")
  }

  // A freed slot must not end a lookup: find two keys that start at the
  // same slot, then remove the first one.
  #[test] fn lookup_passes_over_a_freed_slot() {
    mut m := HashMap::<i32, i32>::with_capacity(4)
    cap := m.capacity()
    first := 0
    mut second := 1
    while slot_of(second, cap) != slot_of(first, cap) {
      second = second + 1
    }
    _ := m.insert(first, 1)
    _ := m.insert(second, 2)
    assert(m.remove(first).is_some(), "the first key was inserted")
    assert(value_at(&m, second) == 2, "a key whose probe passed the freed slot is still found")
  }

  #[test] fn removals_do_not_hide_the_keys_that_remain() {
    mut m := HashMap::<i32, i32>::new()
    mut i := 0
    while i < 30 {
      _ := m.insert(i, i)
      i = i + 1
    }
    mut gone := 0
    while gone < 30 {
      _ := m.remove(gone)
      gone = gone + 3
    }
    mut fresh := 0
    while fresh < 30 {
      _ := m.insert(30 + fresh, fresh)
      fresh = fresh + 1
    }
    assert(m.len() == 50, "twenty survivors plus thirty new keys")
    mut j := 0
    while j < 30 {
      if j % 3 == 0 {
        assert(m.get(j).is_none(), "a removed key stays removed")
      } else {
        assert(value_at(&m, j) == j, "a survivor is still found with its value")
      }
      j = j + 1
    }
    mut t := 0
    while t < 30 {
      assert(value_at(&m, 30 + t) == t, "a key inserted after the removals is found")
      t = t + 1
    }
  }

  #[test] fn value_written_through_get_mut_is_the_one_read_back() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    slot := m.get_mut(0).or_panic("the key is there")
    *slot = 7
    assert(value_at(&m, 0) == 7, "the write lands in the entry")
  }

  #[test] fn get_or_insert_keeps_an_existing_value() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    slot := m.get_or_insert(0, 2)
    kept := *slot
    assert(kept == 1, "a key already present keeps the value it had")
    assert(m.len() == 1, "the call adds nothing for a key that is there")
  }

  #[test] fn get_or_insert_fills_an_absent_key() {
    mut m := HashMap::<i32, i32>::new()
    slot := m.get_or_insert(0, 2)
    got := *slot
    assert(got == 2, "the entry holds the default the call was given")
    assert(value_at(&m, 0) == 2, "the inserted key is there afterwards")
    assert(m.len() == 1, "the call inserts one entry")
  }

  #[test] fn entry_or_insert_fills_only_the_vacant_key() {
    mut m := HashMap::<i32, i32>::new()
    _ := m.insert(0, 1)
    occupied := m.entry(0).or_insert(2)
    kept := *occupied
    assert(kept == 1, "the occupied entry keeps its value")
    vacant := m.entry(1).or_insert(3)
    filled := *vacant
    assert(filled == 3, "the vacant entry takes the default")
    assert(m.len() == 2, "the second call adds an entry")
  }

  #[test] fn maps_with_the_same_entries_compare_equal() {
    mut a := HashMap::<i32, i32>::new()
    _ := a.insert(0, 1)
    _ := a.insert(1, 2)
    mut b := HashMap::<i32, i32>::new()
    _ := b.insert(1, 2)
    _ := b.insert(0, 1)
    assert(a == b, "insertion order does not matter")
    _ := b.insert(2, 3)
    assert(!(a == b), "an extra key makes the maps differ")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }

  fn slot_of(key: i32, cap: usize) -> usize {
    ret (key.hash() as usize) % cap
  }

  fn value_at(m: &HashMap < i32, i32 >, key: i32) -> i32 {
    ret *m.get(key).or_panic("no entry for the key")
  }

  fn value_at_str(m: &HashMap < String, i32 >, key: String) -> i32 {
    ret *m.get(key).or_panic("no entry for the key")
  }
}
