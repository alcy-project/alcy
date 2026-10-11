// `for x in v.iter()` borrows the vector while the cursor lives, and
// the vector is usable again once the loop ends.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  mut total := 0
  for x in v.iter() {
    total = total + *x
  }
  if total != 6 {
    ret 1
  }
  if v.len() != 3 {
    ret 2
  }
  v.push(4)
  if *v.at(3).unwrap() != 4 {
    ret 3
  }
  ret 0
}
