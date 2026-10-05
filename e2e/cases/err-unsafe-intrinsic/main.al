// An intrinsic whose precondition the compiler cannot check is
// declared unsafe, and its call sites need the gate too.
unsafe intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

fn main() {
  mut a := 1u8
  b := 2u8
  memcopy(&mut a, &b, 1)
  _ := a
}
