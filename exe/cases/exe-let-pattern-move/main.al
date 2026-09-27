enum E { A(&mut i32), B }

fn propagates(e: E) -> E {
  v := e?
  ret E::A(v)
}

fn main() -> i32 {
  mut n := 41
  r := propagates(E::A(&mut n))
  a := match r { E::A(p) => *p, E::B => 0 }

  e := E::A(&mut n)
  b := if E::A(p) := e { *p } else { 0 }

  mut e2 := E::A(&mut n)
  mut steps := 0
  while E::A(p) := e2 {
    steps = steps + 1
    _ := p
    break
  }
  ret a + b + steps - 83
}
