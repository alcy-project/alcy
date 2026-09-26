struct Plain { n: i32 }

impl Plain {
  fn drop(self: Plain) {
  }
}

fn main() {
  p := Plain { n: 1 }
  _ := p.n
}
