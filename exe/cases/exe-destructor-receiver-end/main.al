// A destructor's receiver ends from its body: the compiler places no
// end on the receiver or on its fields, so the body ends what it still
// holds (docs/spec/values.md, ADR-0137).

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
  fn drop(mut self: Holder) {
    println("DROP Holder {}", self.text.as_str())
    unsafe { drop_in_place(&mut self.payload) }
    unsafe { drop_in_place(&mut self.text) }
  }
}

fn main() -> i32 {
  h := Holder { payload: Payload { tag: 5 }, text: String::from("held") }
  println("after")
  ret 0
}
