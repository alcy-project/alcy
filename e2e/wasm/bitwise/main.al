// The bitwise operators answer the same word on both backends.
fn main() -> i32 {
  if 12 & 10 != 8 {
    ret 1
  }
  if 12 | 10 != 14 {
    ret 2
  }
  if 12 ^ 10 != 6 {
    ret 3
  }
  if (0 - 1) & 255 != 255 {
    ret 4
  }
  if (0 - 1) ^ (0 - 1) != 0 {
    ret 5
  }
  if 1 << 31 >= 0 {
    ret 6
  }
  if 1 << 30 != 1073741824 {
    ret 7
  }
  ret 0
}
