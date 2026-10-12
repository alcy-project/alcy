// `impl str`: length, emptiness, bytes, and byte iteration.

fn main() -> i32 {
  s := "abc"
  if s.len() != 3 {
    ret 1
  }
  if s.is_empty() {
    ret 2
  }
  if s.byte(1) != 98 {
    ret 3
  }
  mut total := 0
  for b in s {
    total = total + (b as i32)
  }
  if total != 97 + 98 + 99 {
    ret 4
  }
  ret 0
}
