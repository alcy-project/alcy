// A method that takes its receiver by value moves a field out of its
// owner.
struct S {
  b: Option<Vec<i32>>,
}

fn main() -> i32 {
  mut items := Vec::<i32>::new()
  items.push(7)
  s := S { b: Option::Some(items) }
  if !s.b.is_some() {
    ret 1
  }
  if s.b.unwrap().len() != 1 {
    ret 2
  }
  ret 0
}
