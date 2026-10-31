// A request past the instrument's ceiling is still the runtime's panic:
// the sanitizers answer null rather than aborting, which is what the run
// environment asks of them, so an ordinary build and an instrumented one
// report the same way. The size sits between the two ceilings: below half
// the pointer's range, where the runtime asks the host allocator, and
// above the 1 TiB an instrumented run will attempt.

fn main() -> i32 {
  v := Vec::<u8>::with_capacity(35184372088832u64)
  ret v.len() as i32
}
