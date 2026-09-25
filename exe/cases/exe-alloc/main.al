// Exercises the heap intrinsics: typed allocation, unique ownership, a
// round trip through a function boundary, and release.
struct Buf { data: &mut MaybeUninit<u8>, cap: usize }

fn make(cap: usize) -> Buf {
  ret Buf { data: alloc::<u8>(cap), cap: cap }
}

fn main() -> i32 {
  mut bad := 0
  a := make(16)
  if a.cap != 16 {
    bad = 1
  }
  dealloc(a.data, a.cap)

  b := make(32)
  c := make(64)
  if b.cap != 32 {
    bad = 2
  }
  if c.cap != 64 {
    bad = 3
  }
  if b.data == c.data {
    bad = 4
  }
  dealloc(b.data, b.cap)
  dealloc(c.data, c.cap)

  // A zero-size request still yields a distinct freeable pointer.
  z := make(0)
  dealloc(z.data, 0)
  ret bad
}
