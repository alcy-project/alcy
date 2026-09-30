// `Map<V>` keeps its keys by value: a key built outside survives the
// source buffer being overwritten, growth rehashes every live entry,
// and removal leaves a tombstone that a later insert reuses.
fn key(i: i32) -> String {
  mut s := String::with_capacity(2 as usize)
  s.push((48 + i / 10) as u8)
  s.push((48 + i % 10) as u8)
  ret s
}

fn main() -> i32 {
  mut m := Map::<i32>::new()
  if !m.is_empty() {
    ret 1
  }
  if m.get("a").is_some() {
    ret 2
  }
  _ := m.insert("a", 1i32)
  _ := m.insert("b", 2i32)
  if m.len() != 2 {
    ret 3
  }
  if *m.get("a").unwrap() != 1 {
    ret 4
  }
  mut q := m.get_mut("b").unwrap()
  *q = 20i32
  if *m.get("b").unwrap() != 20 {
    ret 5
  }
  // Overwriting an existing key reports the old value and keeps the
  // length.
  if m.insert("a", 3i32).unwrap() != 1 {
    ret 6
  }
  if m.len() != 2 {
    ret 7
  }
  if !m.contains("a") {
    ret 8
  }
  if m.remove("a").unwrap() != 3 {
    ret 9
  }
  if m.len() != 1 {
    ret 10
  }
  if m.remove("zz").is_some() {
    ret 11
  }

  // The map owns the key it stores.
  mut src := String::with_capacity(4 as usize)
  src.push_str("name")
  _ := m.insert(src.as_str(), 7i32)
  src.clear()
  src.push_str("xxxx")
  if !m.contains("name") {
    ret 12
  }

  // Enough distinct keys to force several growths.
  mut i := 0
  while i < 50 {
    k := key(i)
    _ := m.insert(k.as_str(), i + 100)
    i = i + 1
  }
  i = 0
  while i < 50 {
    k := key(i)
    if *m.get(k.as_str()).unwrap() != i + 100 {
      ret 13
    }
    i = i + 1
  }
  if m.len() != 52 {
    ret 14
  }

  // Removing every third key opens holes; reinserting refills them.
  i = 0
  while i < 50 {
    if i % 3 == 0 {
      k := key(i)
      if m.remove(k.as_str()).is_none() {
        ret 15
      }
    }
    i = i + 1
  }
  if m.len() != 52 as usize - 17 {
    ret 16
  }
  i = 0
  while i < 50 {
    if i % 3 == 0 {
      k := key(i)
      _ := m.insert(k.as_str(), i + 200)
    }
    i = i + 1
  }
  i = 0
  while i < 50 {
    k := key(i)
    mut want := i + 100
    if i % 3 == 0 {
      want = i + 200
    }
    if *m.get(k.as_str()).unwrap() != want {
      ret 17
    }
    i = i + 1
  }

  m.clear()
  if !m.is_empty() {
    ret 18
  }
  if m.contains("name") {
    ret 19
  }
  ret 0
}
