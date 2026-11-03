// A field of reference type is a place the holder's referent does
// not stand for: reading `&h.buf` yields the reference the field
// stores, which points at the buffer, not at the holder.

pub struct Holder {
  pub buf: &mut i32,
}

pub fn get(h: &Holder) -> &i32 {
  ret h.buf
}
