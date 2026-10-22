// An assignment in a comp block reaches a field or an element, not
// only a bare binding (docs/spec/comp.md): the engine evaluates the
// place through the binding and the value stays comp-known.
struct Inner {
  x: i32,
  y: i32,
}

struct Outer {
  inner: Inner,
  z: i32,
}

comp fn c_value(comp n: i32) -> i32 {
  ret comp {
    mut o := Outer { inner: Inner { x: n, y: 2 }, z: 3 }
    o.inner.x = o.inner.x + o.inner.y
    mut m := [[1, 2], [3, 4]]
    m[0][1] = n
    mut t := (1, 2)
    t.0 = n
    o.inner.x + m[0][1] + t.0
  }
}

fn r_value(n: i32) -> i32 {
  mut o := Outer { inner: Inner { x: n, y: 2 }, z: 3 }
  o.inner.x = o.inner.x + o.inner.y
  mut m := [[1, 2], [3, 4]]
  m[0][1] = n
  mut t := (1, 2)
  t.0 = n
  ret o.inner.x + m[0][1] + t.0
}

fn main() -> i32 {
  const five := c_value(5)
  if five != r_value(5) {
    ret 1
  }
  const zero := c_value(0)
  if zero != r_value(0) {
    ret 2
  }
  ret 0
}
