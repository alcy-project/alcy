// Matching a field that owns its payload moves it out of the struct.
struct S {
  b: Option<Vec<i32>>,
}

fn main() -> i32 {
  mut items := Vec::<i32>::new()
  items.push(7)
  s := S { b: Option::Some(items) }
  match s.b {
    Option::Some(inner) => {
      if inner.len() != 1 {
        ret 1
      }
      if *inner.at(0).or_panic_with_message("the vector holds one element") != 7 {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  ret 0
}
