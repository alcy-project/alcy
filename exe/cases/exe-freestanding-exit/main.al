// The exit status crosses to the kernel through the raw syscall the
// compiler's `_start` emits.
fn main() -> i32 {
  ret 7
}
