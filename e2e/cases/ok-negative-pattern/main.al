fn classify(n: i32) -> i32 {
  match n {
    -1 => ret 10,
    0 => ret 20,
    1 => ret 30,
    _ => ret 40,
  }
}

fn main() {
  a := classify(-1)
  b := classify(0)
  c := classify(1)
  d := classify(2)
  _ := a
  _ := b
  _ := c
  _ := d
}
