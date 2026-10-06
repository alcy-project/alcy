spec Shown {
  fn show(self: &Self) -> i32;
}

struct Tag {
  n: i32,
}

impl Shown for Tag {
  fn show(self: &Self) -> i32 {
    ret self.n
  }
}

fn main() -> i32 {
  t := Tag { n: 7 }
  ret t.show() - 7
}
