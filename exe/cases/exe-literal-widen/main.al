fn widen() -> i64 {
  ret 1 + 2i64
}

fn narrow() -> u8 {
  ret 1 + 2u8
}

fn main() -> i32 {
  w := widen()
  n := narrow()
  ret (w as i32) + (n as i32) - 6
}
