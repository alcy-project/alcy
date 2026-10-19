// A deep nest of generic calls: each call resolves once per expression
// and instantiation, so resolving one level does not re-resolve every
// level below it.

enum E { A, B }

fn wrap<T>(x: T) -> Option<T> {
  ret Option::Some(x)
}

fn main() -> i32 {
  o := wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(wrap(E::A))))))))))))))))))))
  ret match o {
    Option::Some(_) => 0,
    Option::None => 1,
  }
}
