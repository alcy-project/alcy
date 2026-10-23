// A `const` item names no storage, so a mutable borrow that reaches
// one - directly, through a field, or through an element - would write
// to a temporary and vanish. The analyzer refuses it the way it refuses
// a plain assignment; the shared form reads the value and stays legal.
struct C {
  n: i32,
}

const K: C = C { n: 1 }

const A: [i32; 2] = [1, 2]

const N: i32 = 7

fn main() -> i32 {
  a := &mut N
  *a = 9
  b := &mut K.n
  *b = 9
  c := &mut A[0]
  *c = 9
  ret 0
}
