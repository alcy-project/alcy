fn main() -> i32 {
  mut a := 41u8
  b := 1u8
  memcopy(&mut a, &b, 1)
  ret a as i32 - b as i32
}
