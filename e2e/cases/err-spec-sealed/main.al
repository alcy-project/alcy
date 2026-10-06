use acme_base::sealed::Sealed;

struct Crate {
  size: i32,
}

impl Sealed for Crate {
  fn seal(self: &Self) -> i32 {
    ret self.size
  }
}

fn main() -> i32 {
  ret 0
}
