struct Point70 {
  x: i32,
  y: i32,
}

impl Point70 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape70 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area70(s: Shape70) -> i32 {
  match s {
    Shape70::Circle(r) => { ret r * 3 * r }
    Shape70::Rect(w, h) => { ret w * h }
    Shape70::Empty => { ret 0 }
  }
}

fn walk70(p: Point70) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
