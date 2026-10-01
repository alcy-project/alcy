pub spec Show {
  fn show(self: &Self) -> str;
}

pub struct Point {
  x: i32,
}

impl Show for Point {
  fn show(self: &Self) -> str {
    ret "point"
  }
}
