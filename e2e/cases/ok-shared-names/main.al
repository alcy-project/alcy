struct S { n: i32 }

impl S {
  fn get(self: &Self) -> i32 { ret self.n }
}

fn get() -> i32 { ret 7 }

fn main() -> i32 { ret get() + S { n: 1 }.get() }
