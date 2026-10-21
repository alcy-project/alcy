// A generic whose return position is an owning closure has one witness
// per instantiation: the closure's type depends on the type argument, so
// two instantiations of one declaration must not share a witness.
fn make<T>(x: T) -> fn() -> T {
  ret [x] () -> x
}

fn main() -> i32 {
  a := make::<i32>(7)
  b := make::<bool>(true)
  if a() != 7 { ret 1 }
  if b() { ret 0 }
  ret 2
}
