// A type with its own destructor still ends the fields it holds: the
// destructor runs first, then the fields of its receiver end in the
// same order (docs/spec/values.md).

struct Payload {
  tag: i32,
}

impl Payload {
  fn drop(self: Payload) {
    println("DROP Payload {}", self.tag)
  }
}

struct Holder {
  payload: Payload,
  text: String,
}

impl Holder {
  fn drop(self: Holder) {
    println("DROP Holder {}", self.text.as_str())
  }
}

fn main() -> i32 {
  h := Holder { payload: Payload { tag: 5 }, text: String::from("held") }
  println("after")
  ret 0
}
