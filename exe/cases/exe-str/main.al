fn count_a(comp s: str) -> i32 {
  ret comp {
    mut n := 0
    mut i := 0usize
    while i < str_len(s) {
      if str_byte(s, i) == 97u8 {
        n = n + 1
      }
      i = i + 1
    }
    n
  }
}

fn main() -> i32 {
  s := "banana"
  part := str_slice(s, 0, 2)
  ret count_a("banana") + str_len(str_slice(s, 1, 4)) as i32 - str_len(s) as i32 + str_byte(part, 0) as i32 - 98
}
