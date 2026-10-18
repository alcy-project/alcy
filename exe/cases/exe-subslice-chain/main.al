// A method call whose receiver is a run. `s[run]` names a view, so a
// reference receiver takes that view rather than the address of an
// element: reading it as a place built an element access out of a
// Range value, which aborted the emitter.
fn main() -> i32 {
  a := [10i32, 20i32, 30i32]
  s := a.as_slice()
  if s[1..<3].len() != 2 { ret 1 }
  if s[1..<3].first().is_some() != true { ret 2 }
  if *s[1..<3].at(0).unwrap() != 20i32 { ret 3 }
  if s[..<2].len() != 2 { ret 4 }
  if s[1..].len() != 2 { ret 5 }
  if s[..].len() != 3 { ret 6 }
  if s[0..=2].len() != 3 { ret 7 }
  t := "hello"
  if t[1..<3].len() != 2 { ret 8 }
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(2i32)
  v.push(3i32)
  w := v.as_slice()
  if w[1..<3].len() != 2 { ret 9 }
  if w[1..<3].last().is_some() != true { ret 10 }
  ret 0
}
