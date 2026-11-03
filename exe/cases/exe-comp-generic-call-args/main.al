struct Boxed<T> {
  v: T,
}

comp fn pick<T>(x: T) -> T {
  ret x
}

fn main() -> i32 {
  const n := comp { pick::<i32>(21) }
  const b := comp { pick::<Boxed<i32>>(Boxed { v: 21 }) }
  ret (n + b.v) - 42
}
