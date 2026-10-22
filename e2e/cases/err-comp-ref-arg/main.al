// A reference is not a type a comp-known value can have, and a borrow
// of a const is what the expression rule alone admits.
comp fn deref(comp x: &i32) -> i32 {
  ret *x
}

const K: i32 = 5

fn main() {
  const y := deref(&K)
  _ := y
}
