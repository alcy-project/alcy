// `Set` is a thin wrapper over `HashMap<String, u8>`. Its methods reach the map
// through `self.map`, so the wrapper also pins that a method on a
// field borrows the field rather than a copy of it: a copy would
// insert into a temporary and leave the set empty.
fn key(i: i32) -> String {
  mut s := String::with_capacity(2 as usize)
  s.push((48 + i / 10) as u8)
  s.push((48 + i % 10) as u8)
  ret s
}

fn main() -> i32 {
  mut s := HashSet::<String>::new()
  if !s.is_empty() {
    ret 1
  }
  if !s.insert(String::from("x")) {
    ret 2
  }
  if s.insert(String::from("x")) {
    ret 3
  }
  if !s.contains(String::from("x")) {
    ret 4
  }
  if s.len() != 1 {
    ret 5
  }
  if !s.remove(String::from("x")) {
    ret 6
  }
  if s.remove(String::from("x")) {
    ret 7
  }
  if !s.is_empty() {
    ret 8
  }

  // Keys built in a callee outlive the callee's string, and the set
  // grows past its first table.
  mut i := 0
  while i < 40 {
    k := key(i)
    if !s.insert(k) {
      ret 9
    }
    i = i + 1
  }
  if s.len() != 40 {
    ret 10
  }
  i = 0
  while i < 40 {
    if i % 2 == 0 {
      k := key(i)
      if !s.remove(k) {
        ret 11
      }
    }
    i = i + 1
  }
  if s.len() != 20 {
    ret 12
  }
  if s.contains(String::from("00")) {
    ret 13
  }
  if !s.contains(String::from("01")) {
    ret 14
  }

  s.clear()
  if !s.is_empty() {
    ret 15
  }
  if s.contains(String::from("01")) {
    ret 16
  }
  ret 0
}
