// The collections hold up under churn: growth, removal, re-insertion,
// owning values, and sliding windows, all the way through the drop
// glue. The sanitized build runs the same program with the address
// and undefined-behavior sanitizers on.
fn main() -> i32 {
  // A thousand keys, then every third one removed, by value.
  mut m := Map::<i32>::new()
  mut i := 0
  while i < 1000 {
    k := format("key{}", i)
    _ := m.insert(k.as_str(), i)
    i = i + 1
  }
  if m.len() != 1000 { ret 1 }
  mut j := 0
  while j < 1000 {
    if j % 3 == 0 {
      k := format("key{}", j)
      _ := m.remove(k.as_str())
    }
    j = j + 1
  }
  // 1000 - 334 = 666 keys remain; their values sum to
  // (0 + ... + 999) - 3 * (0 + ... + 333) = 499500 - 166833.
  if m.len() != 666 { ret 2 }
  mut t := 0
  for (k, v) in m.iter() {
    t = t + *v
  }
  if t != 332667 { ret 3 }

  // A sliding window of owning strings: each push may reallocate and
  // each removal drops, so the glue runs at every size.
  mut v := Vec::<String>::new()
  mut x := 0
  while x < 500 {
    v.push(format("value-{}", x))
    if v.len() > 64 {
      _ := v.swap_remove(0)
    }
    x = x + 1
  }
  if v.len() != 64 { ret 4 }
  mut ok := true
  for e in v.iter() {
    if !e.starts_with("value-") { ok = false }
  }
  if !ok { ret 5 }

  // Owning values end with the map.
  mut m2 := Map::<String>::new()
  mut y := 0
  while y < 300 {
    k := format("k{}", y)
    _ := m2.insert(k.as_str(), format("v{}", y))
    y = y + 1
  }
  if m2.len() != 300 { ret 6 }

  // A set of eight hundred keys.
  mut s := Set::new()
  mut z := 0
  while z < 800 {
    k := format("s{}", z)
    _ := s.insert(k.as_str())
    z = z + 1
  }
  if s.len() != 800 { ret 7 }
  ret 0
}
