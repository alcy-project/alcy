spec Display {
  fn show(self: &Self) -> str;
}
struct Point {
  x: i32,
}
impl Display for Point {
  fn show(self: &Self) -> str {
    ret "point"
  }
}
fn main() -> i32 {
  p := Point { x: 1 }
  s := p.show()
  if str_len(s) != 5 { ret 1 }
  ret 0
}
