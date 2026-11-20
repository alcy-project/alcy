// A `reserve` whose request the length cannot be added to is refused:
// the wrapped sum would leave the room short of the request, so every
// element past the block would be written outside it.
fn main() -> i32 {
  mut v := Vec::<u8>::new()
  v.push(1u8)
  v.reserve(wrapping_sub(0 as usize, 1 as usize))
  ret 0
}
