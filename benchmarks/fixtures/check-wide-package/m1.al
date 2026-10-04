struct Point1 {
  x: i32,
  y: i32,
}

impl Point1 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape1 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area1(s: Shape1) -> i32 {
  match s {
    Shape1::Circle(r) => { ret r * 3 * r }
    Shape1::Rect(w, h) => { ret w * h }
    Shape1::Empty => { ret 0 }
  }
}

fn walk1(p: Point1) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
