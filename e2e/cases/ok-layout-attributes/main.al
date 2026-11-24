// Layout settings choose a discriminant width and cap the alignment
// used to place a zero-sized trailing field.
#[layout(min_align = 16, max_align = 16)]
struct Wide {
  x: u8,
}

#[layout(tag_size = 1)]
enum Flag {
  Off,
  On,
}

#[layout(tag_size = 1)]
enum Tagged {
  Small(i32),
  Large(i32),
}

#[layout(tag_size = 8)]
enum WideTag {
  Off,
  On,
}

struct Offset {
  a: u8,
  #[layout(max_align = 1)] tail: [u16; 0],
  c: u8,
}

enum OneValue {
  Only,
}

struct Packed {
  a: u8,
  #[layout(max_align = 1)] tail: [u16; 0],
}

fn pick(f: Flag) -> i32 {
  match f {
    Flag::Off => ret 1,
    Flag::On => ret 2,
  }
}

fn read_tagged(t: Tagged) -> i32 {
  match t {
    Tagged::Small(v) => ret v,
    Tagged::Large(v) => ret v + 100,
  }
}

fn read_offset(s: Offset) -> i32 {
  ret s.a as i32 + s.c as i32
}

fn pick_one(v: OneValue) -> i32 {
  match v {
    OneValue::Only => ret 7,
  }
}

fn main() -> i32 {
  if align_of::<Wide>() != 16 {
    ret 1
  }
  if size_of::<Wide>() != 16 {
    ret 2
  }
  if size_of::<Flag>() != 1 {
    ret 3
  }
  if size_of::<Packed>() != 1 {
    ret 4
  }
  if pick(Flag::Off) != 1 {
    ret 5
  }
  if pick(Flag::On) != 2 {
    ret 6
  }
  ret 0
}
