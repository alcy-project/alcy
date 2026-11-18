// `#[when(...)]` drops the arms this target does not name; the OS
// arms hold the same fn because only one survives per target.
#[when(target.os == "linux")]
fn present() -> i32 {
  ret 0
}

#[when(target.os == "windows")]
fn present() -> i32 {
  ret 0
}

#[when(target.os == "darwin")]
fn present() -> i32 {
  ret 0
}

#[when(target.os == "wasi" || target.os == "unknown")]
fn present() -> i32 {
  ret 0
}

fn main() -> i32 {
  ret present()
}
