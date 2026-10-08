// Value parameters instantiate a declaration per value: the count is
// part of the type, so `ArrayVec<i32, 4>` and a `const` naming 4 are
// one instance while 8 is another (ADR-0054).

struct ArrayVec<T, N: usize> {
  data: [T; N],
  len: usize
}

enum Chunk<T, N: usize> {
  Full([T; N]),
  Empty
}

const K: usize = 4

fn main() -> i32 {
  a: ArrayVec<i32, 4> := ArrayVec { data: [1, 2, 3, 4], len: 0 }
  b: ArrayVec<i32, K> := a
  c: ArrayVec<i32, 8> := ArrayVec { data: [0, 0, 0, 0, 0, 0, 0, 0], len: 8 }
  e: Chunk<u8, 2> := Chunk::Full([7 as u8, 8 as u8])
  n := match e {
    Chunk::Full(x) => x[0] as i32,
    Chunk::Empty => 0,
  }
  ret a.data[3] + b.data[0] + c.data[7] + n
}
