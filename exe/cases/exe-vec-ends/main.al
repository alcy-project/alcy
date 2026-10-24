// `first`/`last` answer the ends of the sequence.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  if v.first().is_some() {
    ret 1
  }
  if v.last().is_some() {
    ret 2
  }
  v.push(10)
  if *v.first().or_panic_with_message("expected a value") != 10 {
    ret 3
  }
  if *v.last().or_panic_with_message("expected a value") != 10 {
    ret 4
  }
  v.push(20)
  v.push(30)
  if *v.first().or_panic_with_message("expected a value") != 10 {
    ret 5
  }
  if *v.last().or_panic_with_message("expected a value") != 30 {
    ret 6
  }
  ret 0
}
