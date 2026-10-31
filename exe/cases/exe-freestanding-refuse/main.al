// The freestanding allocator adds the alignment and its two-word header
// to the size before mapping it, so a size at the top of the pointer's
// range would wrap into a block far smaller than the caller asked for.
// The allocator refuses it instead.

fn main() -> i32 {
  v := Vec::<u8>::with_capacity(18446744073709551615u64)
  ret v.len() as i32
}
