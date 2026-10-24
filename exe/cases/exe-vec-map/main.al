// `Vec::map` answers a new vector of each element passed through the
// closure; the element type is the closure's return type, which the
// call infers from the closure's own signature.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  doubled := v.map((x: &i32) -> *x * 2)
  if *doubled.at(0).or_panic_with_message("expected a value") != 2 {
    ret 1
  }
  if *doubled.at(1).or_panic_with_message("expected a value") != 4 {
    ret 2
  }
  labels := v.map((x: &i32) -> format("n{}", *x))
  if labels.at(0).or_panic_with_message("expected a value").as_str() != "n1" {
    ret 3
  }
  empty := Vec::<i32>::new().map((x: &i32) -> *x)
  if empty.len() != 0 {
    ret 4
  }
  ret 0
}
