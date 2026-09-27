fn classify(n: i32) -> i32 {
  match n {
    -1 => 10,
    0 => 20,
    1 => 30,
    _ => 40,
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
