enum Holder { Ref(&mut i32), Nothing }

// A by-value match consumes a move-only scrutinee, and the arms read it
// to do so: the discriminant, and the payload a binding takes. The move
// has to come after those reads rather than before them.
fn is_empty(h: Holder) -> bool {
  ret match h {
    Holder::Ref(_) => false,
    Holder::Nothing => true,
  }
}

fn read(h: Holder) -> i32 {
  ret match h {
    Holder::Ref(r) => *r,
    Holder::Nothing => 0,
  }
}

fn main() -> i32 {
  mut n := 7i32
  // `Holder` holds an exclusive reference, so it is move-only: each
  // function below gets its own value.
  if !is_empty(Holder::Ref(&mut n)) {
    ret 1
  }
  if is_empty(Holder::Nothing) {
    ret 2
  }
  if read(Holder::Ref(&mut n)) != 7 {
    ret 3
  }
  if read(Holder::Nothing) != 0 {
    ret 4
  }
  ret 0
}
