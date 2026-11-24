// An out-of-range byte_slice answers None, so a comp match can handle it.
fn main() -> i32 {
  const c := comp {
    match "abc".byte_slice(1, 99) {
      Option::Some(_) => 1,
      Option::None => 0,
    }
  }
  ret c
}
