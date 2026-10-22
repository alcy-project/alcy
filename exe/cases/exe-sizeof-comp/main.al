// A layout query folds in a const: the value is the layout the
// lowering already holds.
struct S {
  a: u8,
  b: u64,
}

const SIZE_U32: usize = size_of::<u32>()

const ALIGN_U64: usize = align_of::<u64>()

const SIZE_S: usize = size_of::<S>()

const SIZE_TUPLE: usize = size_of::<(u8, u64)>()

const SIZE_OPTION: usize = size_of::<Option<u32>>()

const SIZE_UNIT: usize = size_of::<()>()

fn main() -> i32 {
  if SIZE_U32 != 4 {
    ret 1
  }
  if ALIGN_U64 != 8 {
    ret 2
  }
  if SIZE_S != 16 {
    ret 3
  }
  if SIZE_TUPLE != 16 {
    ret 4
  }
  if SIZE_OPTION != 8 {
    ret 5
  }
  if SIZE_UNIT != 0 {
    ret 6
  }
  ret 0
}
