// The entry wrapper supplies the slot a large result lands in, so the
// exit code reads the discriminant out of memory (ADR-0140).
fn main() -> Result<(), [u8; 70000]> {
  a := [2u8; 70000]
  if a[69999] == 2 {
    ret Ok(())
  }
  ret Err(a)
}
