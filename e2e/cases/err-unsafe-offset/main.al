// Offsetting is an operation the gate covers, like every other
// intrinsic declared unsafe.
unsafe intrinsic fn ptr_offset<T>(ptr: *T, count: isize) -> *T;

fn main() {
  p := 0 as *i32
  _ := ptr_offset(p, 1)
}
