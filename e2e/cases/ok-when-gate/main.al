// `#[when(...)]` on the line above an item drops it on targets its gate
// excludes; the OS arms hold the same name because exactly one arm
// survives on any target.
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
