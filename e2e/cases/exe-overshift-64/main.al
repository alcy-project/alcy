// A 64-bit shift by 100 is out of range and must panic in a debug build.
fn main() -> i32 {
  n: u64 := 100
  v := 1u64 << n
  ret v as i32
}
