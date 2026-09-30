// `as_slice` views the vector's own buffer, so holding it across a
// `push` that reallocates leaves the view reading the block `push`
// released. The checker has to report the conflict rather than leave
// it to run time.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  s := v.as_slice()
  v.push(2i32)
  ret slice_len(s) as i32
}
