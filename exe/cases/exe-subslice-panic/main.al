// A run past the container's end panics instead of naming it.
fn main() -> i32 {
  a := [10i32, 20i32, 30i32]
  v := &a[1..5]
  ret slice_len(v) as i32
}
