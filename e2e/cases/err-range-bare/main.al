// A run of a fixed array is unsized, so it cannot be a value on its
// own: the borrow is the spelling that names it.
fn main() -> i32 {
  a := [10i32, 20i32, 30i32]
  v := a[0..2]
  ret slice_len(v) as i32
}
