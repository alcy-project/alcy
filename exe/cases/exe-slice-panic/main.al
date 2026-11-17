// A slice read past its length panics the same way an array's does; the
// view's fat pointer carries the length the check reads.

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  s := v.as_slice()
  if slice_len(s) != 1 {
    ret 1
  }
  ret s[3]
}
