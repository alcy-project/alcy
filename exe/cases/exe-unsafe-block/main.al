// The gate is a compile-time boundary: an `unsafe` block changes what
// is allowed, not what is computed, and its value is the block's.
unsafe fn read(p: &MaybeUninit<u8>, i: usize) -> u8 {
  ret unsafe { *uninit_ref(elem_ref(p, i)) }
}

fn main() -> i32 {
  data := unsafe { alloc::<u8>(2) }
  unsafe { uninit_write(elem_ptr(data, 0), 7 as u8) }
  unsafe { uninit_write(elem_ptr(data, 1), 35 as u8) }
  a := unsafe { read(data, 0) }
  b := unsafe { read(data, 1) }
  unsafe { dealloc(data, 2) }
  ret (a as i32 + b as i32) - 42
}
