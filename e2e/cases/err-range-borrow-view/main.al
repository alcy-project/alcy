// A view is already a reference: borrowing one has no place behind
// it, so re-slicing drops the `&` instead.
fn main() -> i32 {
  a := [10i32, 20i32, 30i32]
  s := &a[..]
  t := &s[0..2]
  ret slice_len(t) as i32
}
