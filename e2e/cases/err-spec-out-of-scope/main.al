use self::shapes::Point;

fn main() -> i32 {
  p := Point { x: 1 }
  s := p.show()
  ret str_len(s) as i32
}
