fn main() -> i32 {
  mut buf := [0u8; 4]
  buf[0] = 10u8
  buf[1] = 20u8
  buf[2] = buf[0] + buf[1]
  buf[3] = 12u8
  a := [1i32, 2i32, 3i32]
  ret buf[2] as i32 + buf[3] as i32 - buf[1] as i32 + a[0] + a[2] - 26
}
