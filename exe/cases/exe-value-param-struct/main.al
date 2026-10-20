// A generic struct's turbofish takes a value parameter as a value:
// the declaration is ADR-0054's ArrayVec acceptance case.
struct ArrayVec<T, N: usize> {
  data: [T; N],
  len: usize,
}

fn take(v: ArrayVec<i32, 2>) -> i32 {
  ret v.data[1]
}

fn inferred() -> i32 {
  ret take(ArrayVec { data: [4, 5], len: 2 })
}

fn main() -> i32 {
  v := ArrayVec::<i32, 3> { data: [10, 20, 30], len: 3 }
  if v.data[0] != 10 {
    ret 1
  }
  if v.data[2] != 30 {
    ret 2
  }
  if v.len != 3 {
    ret 3
  }
  if inferred() != 5 {
    ret 4
  }
  ret 0
}
