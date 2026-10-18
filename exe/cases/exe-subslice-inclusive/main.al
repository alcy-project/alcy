// An inclusive end names an element, so the last one is in range: the
// bound the check reads is the element after it, which equals the
// length exactly there. Reading it against the length instead rejected
// the last element and aborted a program that named it.
fn main() -> i32 {
  a := [10i32, 20i32, 30i32]
  s := a.as_slice()
  x := s[0..=2]
  if x.len() != 3 { ret 1 }
  if *x.at(0).unwrap() != 10i32 { ret 2 }
  if *x.at(2).unwrap() != 30i32 { ret 3 }
  y := s[..=2]
  if y.len() != 3 { ret 4 }
  z := s[2..=2]
  if z.len() != 1 { ret 5 }
  if *z.at(0).unwrap() != 30i32 { ret 6 }
  w := s[1..=2]
  if w.len() != 2 { ret 7 }
  if *w.at(1).unwrap() != 30i32 { ret 8 }
  e := s[3..=2]
  if e.len() != 0 { ret 9 }
  mut b := [1i32, 2i32, 3i32, 4i32]
  mut m := &mut b[1..=2]
  m[0] = 20i32
  if b[1] != 20i32 { ret 10 }
  if b[0] != 1i32 { ret 11 }
  if b[3] != 4i32 { ret 12 }
  ret 0
}
