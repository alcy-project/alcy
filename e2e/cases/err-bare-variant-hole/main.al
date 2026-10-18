// A bare name that spells a fieldless variant is a constructor, not a
// binding: `A` leaves `B` with no arm.
enum E {
  A,
  B,
}

fn f(e: E) -> i32 {
  match e {
    A => { ret 1 }
  }
}

fn main() -> i32 {
  ret f(E::B)
}
