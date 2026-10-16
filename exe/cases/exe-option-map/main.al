// Option's own adapters instantiate per explicit type argument:
// `map` and `and_then` change the payload type, `ok_or` lifts into
// `Result` (roadmap: Iterator and Option APIs).

fn inc(x: i32) -> i32 {
  ret x + 1
}

fn small(x: i32) -> Option<u8> {
  if x < 0 {
    ret Option::None
  }
  ret Option::Some(x as u8)
}

fn main() -> i32 {
  a: Option<i32> := Option::Some(41)
  b: Option<i32> := a.map::<i32>(inc)
  if b.unwrap() != 42 {
    ret 1
  }
  absent: Option<i32> := Option::None
  if absent.map::<i32>(inc).is_some() {
    ret 2
  }

  c: Option<i32> := Option::Some(7)
  d: Option<u8> := c.and_then::<u8>(small)
  if d.unwrap() != 7 as u8 {
    ret 3
  }
  e: Option<i32> := Option::Some(-1)
  if e.and_then::<u8>(small).is_some() {
    ret 4
  }

  f: Option<i32> := Option::Some(9)
  g: Result<i32, str> := f.ok_or::<str>("missing")
  h := match g {
    Result::Ok(v) => v,
    Result::Err(_) => 0,
  }
  if h != 9 {
    ret 5
  }
  i: Option<i32> := Option::None
  j: Result<i32, str> := i.ok_or::<str>("missing")
  k := match j {
    Result::Ok(_) => 0,
    Result::Err(m) => str_len(m) as i32,
  }
  if k != 7 {
    ret 6
  }
  ret 0
}
