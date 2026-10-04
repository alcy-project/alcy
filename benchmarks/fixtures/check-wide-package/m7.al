struct Point7 {
  x: i32,
  y: i32,
}

impl Point7 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape7 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area7(s: Shape7) -> i32 {
  match s {
    Shape7::Circle(r) => { ret r * 3 * r }
    Shape7::Rect(w, h) => { ret w * h }
    Shape7::Empty => { ret 0 }
  }
}

fn walk7(p: Point7) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
