struct Point31 {
  x: i32,
  y: i32,
}

impl Point31 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape31 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area31(s: Shape31) -> i32 {
  match s {
    Shape31::Circle(r) => { ret r * 3 * r }
    Shape31::Rect(w, h) => { ret w * h }
    Shape31::Empty => { ret 0 }
  }
}

fn walk31(p: Point31) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
