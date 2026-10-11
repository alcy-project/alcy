// Growing a container moves its initialized prefix as one byte copy,
// and `push_str` reserves once for the whole run instead of growing
// per byte.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  mut i := 0
  // Crosses 4 -> 8 -> 16, so the move runs with elements in the buffer.
  while i < 16 {
    v.push(i)
    i = i + 1
  }
  if v.len() != 16 {
    ret 1
  }
  mut j := 0 as usize
  mut total := 0
  while j < v.len() {
    total = total + v[j]
    j = j + 1
  }
  if total != 120 {
    ret 2
  }

  mut s := String::with_capacity(1)
  s.push_str("ab")
  s.push_str("cdefg")
  if s.len() != 7 {
    ret 3
  }
  if s.as_str() != "abcdefg" {
    ret 4
  }
  ret 0
}
