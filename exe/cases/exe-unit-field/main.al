// The unit type takes no room, so a field of it shares an offset with
// whatever the layout puts next, and a store through it must write
// nothing: the value a `()` expression carries is a placeholder whose
// bytes would land on the field beside it.
struct Holder {
  a: (),
  b: u8,
}

enum HasUnit {
  A(()),
  B,
}

fn takes(u: ()) -> i32 {
  ret 5
}

fn silent() {
  ret
}

fn main() -> i32 {
  mut h := Holder { a: (), b: 9u8 }
  if h.b != 9u8 {
    ret 1
  }
  h.a = ()
  if h.b != 9u8 {
    ret 2
  }
  mut h2 := Holder { a: (), b: 7u8 }
  h2.b = 8u8
  if h2.b != 8u8 {
    ret 3
  }
  if takes(()) != 5 {
    ret 4
  }
  silent()
  t := ((), 1u8, 2u16)
  if t.1 != 1u8 {
    ret 5
  }
  if t.2 != 2u16 {
    ret 6
  }
  mut arr := [(); 4]
  arr[0] = ()
  e := HasUnit::A(())
  match e {
    HasUnit::A(x) => {}
    HasUnit::B => {
      ret 7
    }
  }
  if size_of::<Holder>() != 1 {
    ret 8
  }
  if size_of::<HasUnit>() != 4 {
    ret 9
  }
  ret 0
}
