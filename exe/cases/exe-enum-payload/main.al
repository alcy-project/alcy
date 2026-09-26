enum Tag { One(i32), Two }

// `Tag` holds a payload. The payload lives in the value itself, so
// keeping one across a later call has to keep reading the right bytes;
// a payload behind a pointer into the producing frame would be gone by
// then.
fn make(v: i32) -> Tag {
  ret Tag::One(v)
}

fn read(t: Tag) -> i32 {
  ret match t {
    Tag::One(v) => v,
    Tag::Two => -1,
  }
}

fn discard(t: Tag) {
  _ := read(t)
}

// A differing-shape enum: the area has to hold the widest payload even
// though the first variant carries less. The second variant holds two
// words where the first holds one, so the area is padded to fit.
enum Either { Left(i32), Right((i32, i32)) }

fn wide(a: i32, b: i32) -> Either {
  ret Either::Right((a, b))
}

fn wide_read(e: Either) -> i32 {
  ret match e {
    Either::Left(v) => v,
    Either::Right((x, y)) => x + y,
  }
}

fn main() -> i32 {
  kept := make(42i32)
  // Later calls reuse the frames the earlier ones left behind.
  discard(make(7i32))
  discard(make(9i32))
  if read(kept) != 42 {
    ret 1
  }
  narrow := Tag::One(5i32)
  if read(narrow) != 5 {
    ret 2
  }
  if read(Tag::Two) != -1 {
    ret 3
  }
  w := wide(4i32, 7i32)
  if wide_read(w) != 11 {
    ret 4
  }
  if wide_read(Either::Left(3i32)) != 3 {
    ret 5
  }
  ret 0
}
