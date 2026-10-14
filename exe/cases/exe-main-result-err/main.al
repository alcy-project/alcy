// An `Err` from `main` is an exit status, not a panic: the message
// goes to stderr and the process exits 1.
fn main() -> Result<(), str> {
  ret Result::Err("boom")
}
