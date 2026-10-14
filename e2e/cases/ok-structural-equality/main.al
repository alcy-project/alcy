// A tuple or array compares element-wise when each element is a
// scalar or a thin address the backend compares directly.

fn main() -> i32 {
  if !((1, 2) == (1, 2)) {
    ret 1
  }
  if (1, 2) == (1, 3) {
    ret 2
  }
  if !((1, 2) != (1, 3)) {
    ret 3
  }
  if !((1, (2, 3)) == (1, (2, 3))) {
    ret 4
  }
  if !(([1, 2], 3) == ([1, 2], 3)) {
    ret 5
  }
  if [1, 2] == [1, 3] {
    ret 6
  }
  if !([[1, 2], [3, 4]] == [[1, 2], [3, 4]]) {
    ret 7
  }
  x := 5
  y := 6
  if !((&x, 1) == (&x, 1)) {
    ret 8
  }
  if (&x, 1) == (&y, 1) {
    ret 9
  }
  ret 0
}
