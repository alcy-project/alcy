// A generic body that instantiates another generic function keeps its
// own type parameters: the nested call binds `A`, and `T` must still
// name the outer argument afterwards.
fn id<A>(x: A) -> A {
  ret x
}

fn keep<T>(x: T) -> T {
  _ := id::<u8>(7 as u8)
  z: T := x
  ret z
}

fn main() -> i32 {
  ret keep(42i32) - 42
}
