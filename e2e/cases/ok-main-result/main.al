// `main` may return a two-variant enum whose first variant holds `()`.
// The entry thunk maps the first discriminant to exit code 0.
fn run(ok: bool) -> Result<(), str> {
  if ok {
    ret Result::Ok(())
  }
  ret Result::Err("failed")
}

fn main() -> Result<(), str> {
  _ := run(false)
  ret run(true)
}
