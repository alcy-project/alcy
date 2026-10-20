// A literal past every type it could take is still refused.
fn main() -> i32 {
  x := 3000000000 as u8
  ret x as i32
}
