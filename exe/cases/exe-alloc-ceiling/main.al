// A request past the instrument's ceiling is still the runtime's panic:
// the sanitizers answer null rather than aborting, which is what the run
// environment asks of them, so an ordinary build and an instrumented one
// report the same way. The size sits below half the pointer's range,
// where the runtime hands the request to the host allocator rather than
// refusing it itself, and above any address space a host gives a
// process, so the allocator has to answer null. A request of 32 TiB is
// below that second bound: a host that maps the address space it hands
// out lazily grants it, and the program then ends without a panic.

fn main() -> i32 {
  v := Vec::<u8>::with_capacity(4611686018427387904u64)
  ret v.len() as i32
}
