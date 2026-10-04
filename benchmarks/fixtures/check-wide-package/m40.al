struct Point40 {
  x: i32,
  y: i32,
}

impl Point40 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape40 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area40(s: Shape40) -> i32 {
  match s {
    Shape40::Circle(r) => { ret r * 3 * r }
    Shape40::Rect(w, h) => { ret w * h }
    Shape40::Empty => { ret 0 }
  }
}

fn walk40(p: Point40) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
