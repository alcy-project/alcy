// `binary_search_by` halves a sorted vector.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(3)
  v.push(5)
  v.push(7)
  if v.binary_search_by((x: &i32) -> *x - 5).unwrap() != 2 {
    ret 1
  }
  if v.binary_search_by((x: &i32) -> *x - 1).unwrap() != 0 {
    ret 2
  }
  if v.binary_search_by((x: &i32) -> *x - 7).unwrap() != 3 {
    ret 3
  }
  if v.binary_search_by((x: &i32) -> *x - 4).is_some() {
    ret 4
  }
  mut e := Vec::<i32>::new()
  if e.binary_search_by((x: &i32) -> *x).is_some() {
    ret 5
  }
  ret 0
}
