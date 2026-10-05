// Exercises the heap intrinsics: typed allocation, unique ownership, a
// round trip through a function boundary, alignment, a write-readback
// into the returned block, and release.
struct Buf { data: &mut MaybeUninit<u8>, cap: usize }

fn make(cap: usize) -> Buf {
  ret Buf { data: unsafe { alloc::<u8>(cap) }, cap: cap }
}

fn main() -> i32 {
  mut bad := 0
  a := make(16)
  if a.cap != 16 {
    bad = 1
  }
  unsafe { dealloc(a.data, a.cap) }

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
  unsafe { dealloc(b.data, b.cap) }
  unsafe { dealloc(c.data, c.cap) }

  // The allocator answers for the element's alignment: a byte buffer and
  // a `u64` buffer, each written through and read back, so the block is
  // proven usable rather than only reserved. A wrong alignment reaching
  // the allocator, or a block that comes back unaligned, fails the
  // sanitized run.
  bytes := unsafe { alloc::<u8>(3) }
  unsafe { uninit_write(elem_ptr(bytes, 0), 7 as u8) }
  unsafe { uninit_write(elem_ptr(bytes, 2), 9 as u8) }
  if unsafe { *uninit_assume(elem_ptr(bytes, 0)) } as i32 != 7 {
    bad = 5
  }
  if unsafe { *uninit_assume(elem_ptr(bytes, 2)) } as i32 != 9 {
    bad = 6
  }
  unsafe { dealloc(bytes, 3) }

  wide := unsafe { alloc::<u64>(2) }
  unsafe { uninit_write(elem_ptr(wide, 0), 0x0102030405060708 as u64) }
  unsafe { uninit_write(elem_ptr(wide, 1), 0x1122334455667788 as u64) }
  if unsafe { *uninit_assume(elem_ptr(wide, 0)) } != 0x0102030405060708 as u64 {
    bad = 7
  }
  if unsafe { *uninit_assume(elem_ptr(wide, 1)) } != 0x1122334455667788 as u64 {
    bad = 8
  }
  unsafe { dealloc(wide, 2) }

  // A zero-size request still yields a distinct freeable pointer.
  z := make(0)
  unsafe { dealloc(z.data, 0) }
  ret bad
}
