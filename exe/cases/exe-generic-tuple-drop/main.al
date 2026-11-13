// A generic aggregate's end runs its elements' destructors once each,
// even when the substitution reached them through storage copies.

fn sink_pair<T>(a: T, b: T) {
  p := (a, b)
  q := &p
  ret
}

struct Noisy {
  n: i32,
}

impl Noisy {
  fn drop(self: Noisy) {
    println("sd")
  }
}

fn main() -> i32 {
  sink_pair(Noisy { n: 1 }, Noisy { n: 2 })
  ret 0
}
