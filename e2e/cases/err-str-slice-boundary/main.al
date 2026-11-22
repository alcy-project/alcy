// A raw slice may not cut a multi-byte scalar in two.
fn main() -> i32 {
  broken := str_slice("日", 0, 1)
  ret 0
}
