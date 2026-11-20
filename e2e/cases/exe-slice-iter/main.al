// Slices and arrays iterate by reference: `for x in s.iter()` watches a
// vector's live elements, and `for x in a.iter()` an array's.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  s := v.as_slice()
  mut total := 0
  for x in s.iter() {
    total = total + *x
  }
  if total != 6 {
    ret 1
  }

  a := [4i32, 5i32]
  mut at := 0
  for x in a.iter() {
    at = at + *x
  }
  if at != 9 {
    ret 2
  }
  if a.len() != 2 {
    ret 3
  }
  ret 0
}
