spec Ident {
  fn id(self: &Self) -> i32;
}

struct Point { x: i32 }

impl Ident for Point {
  fn id<U>(self: &Point) -> i32 {
    ret 0
  }
}

fn main() -> i32 {
  ret 0
}
