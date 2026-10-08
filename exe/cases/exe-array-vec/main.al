// A value parameter's count is part of the instance: the storage has
// that many elements, writes stay inside it, and distinct counts are
// distinct types (ADR-0054).

struct ArrayVec<T, N: usize> {
  data: [T; N],
  len: usize
}

impl<T, N: usize> ArrayVec<T, N> {
  fn len(self: &Self) -> usize {
    ret self.len
  }

  fn get(self: &Self, i: usize) -> T {
    ret self.data[i]
  }

  fn push(mut self: &mut Self, v: T) {
    self.data[self.len] = v
    self.len = self.len + 1
  }
}

fn main() -> i32 {
  mut v: ArrayVec<i32, 4> := ArrayVec { data: [0, 0, 0, 0], len: 0 }
  v.data[0] = 10
  v.data[3] = 7
  v.len = 2
  if v.len != 2 {
    ret 1
  }
  if v.data[0] != 10 {
    ret 2
  }
  if v.data[3] != 7 {
    ret 3
  }

  w: ArrayVec<i32, 4> := v
  if w.data[0] != 10 {
    ret 4
  }

  z: ArrayVec<u8, 8> := ArrayVec {
    data: [1 as u8, 2 as u8, 3 as u8, 4 as u8, 5 as u8, 6 as u8, 7 as u8, 8 as u8],
    len: 8,
  }
  if z.data[7] != 8 as u8 {
    ret 5
  }

  // The same methods through the impl: three writes into the four-wide
  // vector, each method resolving its own count.
  mut a: ArrayVec<i32, 4> := ArrayVec { data: [0, 0, 0, 0], len: 0 }
  a.push(10)
  a.push(20)
  a.push(30)
  if a.len() != 3 {
    ret 6
  }
  if a.get(0) != 10 {
    ret 7
  }
  if a.get(2) != 30 {
    ret 8
  }

  // A second count in the same program runs as its own instance.
  mut b: ArrayVec<u8, 8> := ArrayVec {
    data: [0 as u8, 0 as u8, 0 as u8, 0 as u8, 0 as u8, 0 as u8, 0 as u8, 0 as u8],
    len: 0,
  }
  b.push(8 as u8)
  if b.len() != 1 {
    ret 9
  }
  if b.get(0) != 8 as u8 {
    ret 10
  }
  ret 0
}
