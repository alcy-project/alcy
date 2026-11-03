// `get` hands back the buffer `x.buf` names, so writing through
// `*x.buf` while the result is live changes what it reads. The
// function boundary carries no loan on the holder for the field's
// referent, so nothing refuses the write.

use reffalias::helper::Holder;
use reffalias::helper::get;

fn main() -> i32 {
  mut n := 1
  mut x := Holder { buf: &mut n }
  r := get(&x)
  *x.buf = 9
  ret *r
}
