// `with_capacity(n)` reserves at least `n` entries and the growth the
// table does at three quarters occupancy never fires for `n` inserts.
// `Vec` and `String` report exactly the requested room.
fn main() -> i32 {
  mut m := Map::<i32>::with_capacity(8 as usize)
  if m.capacity() < 8 as usize {
    ret 1
  }
  cap := m.capacity()
  _ := m.insert("a", 1)
  _ := m.insert("b", 2)
  _ := m.insert("c", 3)
  _ := m.insert("d", 4)
  _ := m.insert("e", 5)
  _ := m.insert("f", 6)
  _ := m.insert("g", 7)
  _ := m.insert("h", 8)
  if m.len() != 8 as usize {
    ret 2
  }
  if m.capacity() != cap {
    ret 3
  }

  // One more entry crosses three quarters of the reserved slots.
  _ := m.insert("i", 9)
  if m.capacity() == cap {
    ret 4
  }

  mut v := Vec::<i32>::with_capacity(7 as usize)
  if v.capacity() != 7 as usize {
    ret 5
  }
  mut i := 0
  while i < 7 {
    v.push(i)
    i = i + 1
  }
  if v.capacity() != 7 as usize {
    ret 6
  }

  mut s := String::with_capacity(5 as usize)
  if s.capacity() != 5 as usize {
    ret 7
  }
  mut j := 0
  while j < 5 {
    s.push(97u8)
    j = j + 1
  }
  if s.capacity() != 5 as usize {
    ret 8
  }
  ret 0
}
