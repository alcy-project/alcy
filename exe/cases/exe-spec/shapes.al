pub spec Show {
  fn show(self: &Self) -> str;

  fn label(self: &Self) -> str;
}

pub struct Point {
  pub x: i32,
  pub y: i32,
}

impl Show for Point {
  fn show(self: &Self) -> str {
    ret "point"
  }

  fn label(self: &Self) -> str {
    ret "a point"
  }
}

pub struct Wrap<T> {
  pub v: T,
}

impl<T> Show for Wrap<T> {
  fn show(self: &Self) -> str {
    ret "wrapped"
  }

  fn label(self: &Self) -> str {
    ret "a wrap"
  }
}
