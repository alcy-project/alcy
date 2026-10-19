// A second variant under one name resolves to whichever entry a lookup
// reached first, and a generic declaration is no different.
enum E<T> {
  A(T),
  A(T),
}

fn main() -> i32 {
  ret 0
}
