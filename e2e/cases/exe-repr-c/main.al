// `#[repr(C)]` fixes a struct's layout to the platform's C layout; the
// alcy default already satisfies it, and the marker is a contract that
// it will not change.
#[repr(C)]
struct Pair {
  a: u8,
  b: i32,
}

#[repr(C)]
struct Triple {
  a: i64,
  b: u8,
  c: u8,
}

#[repr(C)]
struct Packed {
  a: u8,
  b: u8,
  c: u32,
}

fn main() -> i32 {
  if size_of::<Pair>() != 8 {
    ret 1
  }
  if size_of::<Triple>() != 16 {
    ret 2
  }
  if size_of::<Packed>() != 8 {
    ret 3
  }
  // A field sits at the offset its alignment allows: the second byte
  // starts at 8 after one u8, and the third byte follows it at 9.
  // Nothing moves between `repr(C)` and the default layout today.
  ret 0
}
