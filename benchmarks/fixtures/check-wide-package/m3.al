struct Point3 {
  x: i32,
  y: i32,
}

impl Point3 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape3 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area3(s: Shape3) -> i32 {
  match s {
    Shape3::Circle(r) => { ret r * 3 * r }
    Shape3::Rect(w, h) => { ret w * h }
    Shape3::Empty => { ret 0 }
  }
}

fn walk3(p: Point3) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
