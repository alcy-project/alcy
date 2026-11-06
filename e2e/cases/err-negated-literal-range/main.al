// A negated literal's magnitude must fit the type the position names:
// a signed minimum is as far as a negation reaches, and an unsigned
// type has no negative value at all.
fn main() -> i32 {
  too_small: i8 := -129
  unsigned: u8 := -1
  past_f32: f32 := -1e39
  ret 0
}
