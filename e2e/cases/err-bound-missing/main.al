// A bound says what a parameter answers; an instantiation that does
// not implement it is refused at the call, naming the caller's type
// rather than a line inside the library.

struct Plain {
  n: i32,
}

fn main() {
  mut a: Vec<Plain> := Vec::<Plain>::new()
  mut b: Vec<Plain> := Vec::<Plain>::new()
  a.push(Plain { n: 1 })
  b.push(Plain { n: 2 })
  _ := a == b
}
