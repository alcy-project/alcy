// `insert_str` inserts a whole view at an index, and the index may sit
// at either end.
fn main() -> i32 {
  mut s := format("ab")
  s.insert_str(1, "XY")
  if s.as_str() != "aXYb" {
    ret 1
  }
  s.insert_str(0, "-")
  if s.as_str() != "-aXYb" {
    ret 2
  }
  n := s.len_bytes()
  s.insert_str(n, "!")
  if s.as_str() != "-aXYb!" {
    ret 3
  }
  mut e := format("")
  e.insert_str(0, "z")
  if e.as_str() != "z" {
    ret 4
  }
  // An empty insertion leaves the bytes where they were.
  s.insert_str(2, "")
  if s.as_str() != "-aXYb!" {
    ret 5
  }
  ret 0
}
