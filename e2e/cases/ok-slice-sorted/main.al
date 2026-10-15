// `is_sorted_by` checks a view against an ordering.
fn main() -> i32 {
  a := [1, 2, 3]
  if !a.as_slice().is_sorted_by((x: &i32, y: &i32) -> *x < *y) { ret 1 }
  b := [1, 3, 2]
  if b.as_slice().is_sorted_by((x: &i32, y: &i32) -> *x < *y) { ret 2 }
  mut e := Vec::<i32>::new()
  if !e.as_slice().is_sorted_by((x: &i32, y: &i32) -> *x < *y) { ret 3 }
  one := [7]
  if !one.as_slice().is_sorted_by((x: &i32, y: &i32) -> *x < *y) { ret 4 }
  ret 0
}
