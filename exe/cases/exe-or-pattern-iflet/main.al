enum E { A, B, C }

fn main() -> i32 {
  e := E::B
  r := if E::A | E::B := e { 1 } else { 2 }
  mut i := 0
  while E::A | E::C := e {
    i = i + 1
    break
  }
  ret r - 1
}
