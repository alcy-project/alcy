// The destructor's own body is the one scope that may take its receiver
// apart (ADR-0137).

struct Holder {
  s: String,
  id: i32,
}

impl Holder {
  fn drop(self: Self) {
    Holder { s: t, id: _ } := self
    println("DROP [{}]", t)
  }
}

fn main() {
  h := Holder { s: String::from("abc"), id: 1 }
  println("made")
}
