// A function value is not a generic argument yet: until a bound says
// what a parameter can be called as, an inferred argument cannot have
// a function type.
fn id<T>(x: T) -> T {
  ret x
}

fn one(x: i32) -> i32 {
  ret x
}

fn main() {
  f: (i32) -> i32 := one
  g := id(f)
  ret g(1)
}
