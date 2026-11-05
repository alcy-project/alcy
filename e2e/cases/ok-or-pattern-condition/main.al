// An or-pattern in a condition: each alternative tests in a block of its
// own, and only a full match branches to the body.
enum E {
  A,
  B,
  C(i32),
}

fn main() -> i32 {
  e := E::B
  r := if E::A | E::B := e {
    1
  } else {
    2
  }
  if r != 1 {
    ret 1
  }
  mut i := 0
  while E::A | E::B := e {
    i = i + 1
    break
  }
  if i != 1 {
    ret 2
  }
  d := if E::C(n) := E::C(4) {
    n
  } else {
    0
  }
  if d != 4 {
    ret 3
  }
  ret 0
}
