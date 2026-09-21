struct H { r: &mut i32 }

fn id(x: &i32) -> &i32 {
  ret x
}

fn fst(a: &i32, b: &i32) -> &i32 {
  ret a
}

fn wrap(r: &mut i32, s: &i32) -> H {
  ret H { r: r }
}

fn proj(h: H) -> &mut i32 {
  ret h.r
}

fn pick(n: i32, x: &i32) -> &i32 {
  if n <= 0 {
    ret x
  }
  ret pick(n - 1, x)
}

fn main() {
  p := 1
  q := 2
  a := id(&p)
  b := fst(&p, &q)
  m := &mut q
  mut x := 3
  h := wrap(&mut x, &p)
  r := proj(h)
  s := pick(2, &p)
  _ := a
  _ := b
  _ := m
  _ := r
  _ := s
}
