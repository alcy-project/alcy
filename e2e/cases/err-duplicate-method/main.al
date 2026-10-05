struct S { n: i32 }

impl S {
  fn get(self: &Self) -> i32 { ret self.n }
}

impl S {
  fn get(self: &Self) -> i32 { ret self.n + 1 }
}

fn main() -> i32 { ret S { n: 1 }.get() }
