// The first ABI is scalars, raw pointers, and (); an aggregate is
// refused rather than lowered optimistically.
struct Pair { a: i32, b: i32 }

extern "C" {
  fn takes(p: Pair);
}

fn main() {}
