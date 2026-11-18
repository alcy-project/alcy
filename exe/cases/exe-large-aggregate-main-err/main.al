// A large error variant still exits nonzero with the message the by-value
// form prints.
fn main() -> Result<(), [u8; 70000]> {
  ret Err([3u8; 70000])
}
