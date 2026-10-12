// A bound says what a parameter answers (`T: Display`), and the body
// calls the bound's methods; the check happens at the call, where the
// argument's type is known. A `+` list joins bounds, and an impl may
// bound its own parameters the same way. A builtin convertible
// satisfies `Display` without an implementation.

struct Point {
  x: i32,
  y: i32,
}

impl Display for Point {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    s := format("({}, {})", self.x, self.y)
    ret s.display(out)
  }
}

impl PartialEq for Point {
  fn eq(self: &Self, other: &Self) -> bool {
    ret self.x == other.x
  }
}

impl Eq for Point {
}

fn show<T: Display>(x: T) {
  println("[{}]", x)
}

fn same<T: Display + PartialEq>(a: T, b: T) -> bool {
  println("<{}>", a)
  ret a == b
}

struct Wrap<T> {
  value: T,
}

impl<T: Display> Wrap<T> {
  fn show(self: &Self) {
    println("w({})", self.value)
  }
}

fn main() -> i32 {
  show(3)
  show("hi")
  p := Point { x: 1, y: 2 }
  show(p)

  equal := Point { x: 1, y: 9 }
  if !same(p, equal) {
    ret 1
  }
  other := Point { x: 7, y: 2 }
  if same(p, other) {
    ret 2
  }

  w: Wrap<Point> := Wrap { value: p }
  w.show()
  n: Wrap<i32> := Wrap { value: 5i32 }
  n.show()
  ret 0
}
