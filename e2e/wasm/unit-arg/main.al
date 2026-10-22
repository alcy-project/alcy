// A zero-sized argument takes no wasm words, and the operand the
// lowering carries for it is a placeholder. Pushing that placeholder
// left the call a word deep, so the next store read it as its address:
// the call ran, returned the right value, and wrote elsewhere. The
// zero-sized load and store ride the same shapes.
struct Holder {
  a: (),
  b: u8,
}

fn bump(mut x: &mut i32, u: ()) -> i32 {
  *x = *x + 10
  ret 42
}

fn bump_first(u: (), mut x: &mut i32) -> i32 {
  *x = *x + 100
  ret 43
}

fn main() -> i32 {
  mut a := 0
  if bump(&mut a, ()) != 42 {
    ret 1
  }
  if a != 10 {
    ret 2
  }
  mut b := 0
  if bump_first((), &mut b) != 43 {
    ret 3
  }
  if b != 100 {
    ret 4
  }
  mut h := Holder { a: (), b: 9u8 }
  h.a = ()
  if h.b != 9u8 {
    ret 5
  }
  mut v := Vec::<()>::new()
  v.push(())
  v.push(())
  if v.len() != 2 {
    ret 6
  }
  if v.pop().is_some() != true {
    ret 7
  }
  if v.len() != 1 {
    ret 8
  }
  ret 0
}
