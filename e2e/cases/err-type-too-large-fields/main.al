// Each field fits on its own; their sum does not, and the
// aggregate's layout is the sum.

struct Big {
  a: [u8; 4611686018427387904],
  b: [u8; 4611686018427387904],
  c: [u8; 4611686018427387904],
}

fn main() -> i32 {
  ret size_of::<Big>() as i32
}
