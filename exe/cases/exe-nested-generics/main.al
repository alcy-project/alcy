// Nested generic closings need no space between the brackets.
struct S {
  a: Vec<Vec<i32>>,
  b: Option<Vec<i32>>,
}

fn main() -> i32 {
  mut s := S { a: Vec::<Vec<i32>>::new(), b: Option::Some(Vec::<i32>::new()), }
  s.a.push(Vec::<i32>::new())
  if s.a.len() != 1 {
    ret 1
  }
  if !s.b.is_some() {
    ret 2
  }
  if s.b.is_none() {
    ret 3
  }
  ret 0
}
