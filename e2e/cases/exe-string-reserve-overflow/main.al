// The same refusal over bytes: the sum is formed in the wrapping form,
// so the request past the largest `usize` reaches it rather than
// aborting inside the checked addition that computes the byte count.
fn main() -> i32 {
  mut s := String::with_capacity(2 as usize)
  s.push_str("a")
  s.reserve(wrapping_sub(0 as usize, 1 as usize))
  ret 0
}
