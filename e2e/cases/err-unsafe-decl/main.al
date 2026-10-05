// The declaration states the precondition, and the checker refuses a
// declaration that leaves it out.
intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

fn main() {}
