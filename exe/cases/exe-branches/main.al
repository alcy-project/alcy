enum Shape { Circle(i32), Rect }

fn area(s: Shape) -> i32 {
  r := match s {
    Shape::Circle(x) => x,
    Shape::Rect => 0,
  }
  ret r
}

fn pick(n: i32) -> Optional<i32> {
  if n <= 0 {
    ret None
  }
  ret Some(n * 2)
}

fn main() -> i32 {
  a := area(Shape::Circle(21))
  v := match pick(a) {
    Some(x) => x,
    None => 0,
  }
  if v == 42 {
    println("ok")
    ret 0
  }
  println("bad")
  ret 1
}
