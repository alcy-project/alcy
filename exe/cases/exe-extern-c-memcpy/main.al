// A declaration names an unmangled symbol, and every call goes
// through the opt-in. The hosted build links no C library, so the
// case reaches for a C-ABI symbol the runtime itself defines; the
// path under test is the declaration and the call, not the copy.
extern "C" {
  fn memcpy(dst: *mut u8, src: *u8, len: usize) -> *mut u8;
}

fn main() -> i32 {
  source := [1u8, 2u8, 3u8, 4u8]
  dest := [0u8, 0u8, 0u8, 0u8]
  unsafe { _ := memcpy(&mut dest[0] as *mut u8, &source[0] as *u8, 4 as usize) }
  if dest[3] != 4u8 || dest[0] != 1u8 {
    ret 1
  }
  ret 0
}
