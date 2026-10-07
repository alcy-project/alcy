// `comp fn` calls happen during compilation; the result splices into
// the surrounding code (ADR-0054).

comp fn twice(x: i32) -> i32 {
  ret x * 2
}

const N: i32 = twice(21)

fn main() -> i32 {
  ret comp { twice(1) } + N - 44
}
