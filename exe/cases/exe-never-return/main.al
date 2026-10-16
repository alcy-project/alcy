// A diverging value ends its path where it stands: `ret` of a `!` call
// needs no store after the call, and the panic still aborts with its
// site.
fn boom() -> !{
  panic("never")
}

fn main() -> i32 {
  ret boom()
}
