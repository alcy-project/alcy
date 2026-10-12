// `impl<T> [T]` and `impl<T, N: usize> [T; N]`: a slice view and an
// array both answer their own length.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  s := v.as_slice()
  if s.len() != 2 {
    ret 1
  }
  if s.is_empty() {
    ret 2
  }
  a := [7i32, 8i32, 9i32]
  if a.len() != 3 {
    ret 3
  }
  if a.is_empty() {
    ret 4
  }
  ret 0
}
