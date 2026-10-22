// The comp-known types include composites of comp-known values
// (docs/spec/comp.md): a struct, an array, and a tuple of them stay
// comp-known, and the type rule walks the shape to say so.
struct P { a: i32, b: u8 }
const K: P = P { a: 3, b: 4 }

comp fn sum(comp p: P) -> i32 {
  ret p.a + (p.b as i32)
}

fn main() {
  const x := sum(K)
  const arr := comp { [K.a, K.b as i32] }
  const tup := comp { (K.a, K.b as i32) }
  _ := x + arr[0] + arr[1] + tup.0 + tup.1
}
