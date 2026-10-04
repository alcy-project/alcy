struct Point35 {
  x: i32,
  y: i32,
}

impl Point35 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape35 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area35(s: Shape35) -> i32 {
  match s {
    Shape35::Circle(r) => { ret r * 3 * r }
    Shape35::Rect(w, h) => { ret w * h }
    Shape35::Empty => { ret 0 }
  }
}

fn walk35(p: Point35) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
