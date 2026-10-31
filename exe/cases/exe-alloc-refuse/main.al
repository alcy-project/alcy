// A request the runtime cannot meet is a panic rather than a null
// answer. The size at the top of the pointer's range is more than the
// address space can hold, so the allocator refuses it before the host
// allocator is asked.

fn main() -> i32 {
  v := Vec::<u8>::with_capacity(18446744073709551615u64)
  ret v.len() as i32
}
