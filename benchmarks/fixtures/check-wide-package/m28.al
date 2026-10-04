struct Point28 {
  x: i32,
  y: i32,
}

impl Point28 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape28 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area28(s: Shape28) -> i32 {
  match s {
    Shape28::Circle(r) => { ret r * 3 * r }
    Shape28::Rect(w, h) => { ret w * h }
    Shape28::Empty => { ret 0 }
  }
}

fn walk28(p: Point28) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
