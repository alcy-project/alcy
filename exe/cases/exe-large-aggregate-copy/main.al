// Copying a large aggregate copies the bytes: the binding that follows
// a later write keeps its own value (ADR-0140).
const FILLED: [u8; 5000] = [8u8; 5000]

fn main() -> i32 {
  mut a := [7u8; 5000]
  b := a
  a[0] = 9
  print("{} {}\n", a[0], b[0])
  edge := [1u8; 4096]
  small := [2u8; 4095]
  print("{} {}\n", edge[4095], small[4094])
  global := FILLED
  print("{} {}\n", FILLED[4999], global[4999])
  ret 0
}
