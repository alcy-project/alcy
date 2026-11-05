// The entry wrapper supplies the slot a 70 KiB result lands in, and that
// slot takes shadow stack like a frame does: it counts against the
// reserve, and it checks the reserve before it takes the bytes.
fn main() -> Result<(), [u8; 70000]> {
  a := [5u8; 70000]
  print("{}\n", a[69999])
  if a[69999] == 5 {
    ret Ok(())
  }
  ret Err(a)
}
