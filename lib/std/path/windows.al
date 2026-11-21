// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Paths over wide units, the platform's own spelling. The units are
// UTF-16 whenever the path came from text and kept as they are
// otherwise: a path is what the operating system was handed.

use core::string::is_utf8;
use core::string::from_utf8;

#[when(target.os == "windows")]
pub struct Path {
  wide: Vec<u16>,
}

#[when(target.os == "windows")]
impl Path {
  // The path a string names, its bytes as UTF-16 units.
  pub fn from_str(s: str) -> Path {
    ret Path { wide: encode(String::from(s).as_bytes()) }
  }

  // The path the bytes name, or `None` when they are not UTF-8. The
  // check is what `to_str` undoes, so a path always round-trips.
  pub fn from_bytes(bytes: &[u8]) -> Option<Path> {
    match from_utf8(bytes) {
      Result::Ok(text) => {
        ret Option::Some(Path::from_str(text))
      }
      Result::Err(_) => {
        ret Option::None
      }
    }
  }

  // The bytes the path is made of, as the kernel reads them: UTF-16
  // little endian, two per unit.
  pub fn len_bytes(self: &Self) -> usize {
    ret self.wide.len() * 2
  }

  // The wide units the path is made of, borrowed.
  pub fn as_wide(self: &Self) -> &[u16] {
    ret self.wide.as_slice()
  }

  // A copy of the bytes the path is made of, UTF-8 encoded, for the
  // calls that read them by value.
  pub fn encoded_bytes(self: &Self) -> Vec<u8> {
    match decode(self.as_wide()) {
      Option::Some(bytes) => {
        ret bytes
      }
      Option::None => {
        panic("the path units are not scalar-shaped")
      }
    }
  }

  // The path checked as text, or `None` when its units are not
  // scalar-shaped.
  pub fn to_str(self: &Self) -> Option<String> {
    match decode(self.as_wide()) {
      Option::Some(bytes) => {
        ret String::from_slice(bytes.as_slice())
      }
      Option::None => {
        ret Option::None
      }
    }
  }

  // Whether the path names a root: a drive root, a UNC prefix, or
  // either separator opening the path. A letter and a colon alone name
  // the current directory on that drive, so a separator has to follow
  // the colon before the path is absolute.
  pub fn is_absolute(self: &Self) -> bool {
    n := self.wide.len()
    if n == 0 {
      ret false
    }
    first := self.wide[0]
    if first == 47u16 || first == 92u16 {
      ret true
    }
    if n >= 3 && self.wide[1] == 58u16 && is_separator(self.wide[2]) {
      ret true
    }
    ret false
  }

  // The path with `other` joined onto the end. An absolute `other`
  // replaces the whole path; otherwise one separator is inserted
  // between two components that are not already separated.
  pub fn join(self: &Self, other: &Path) -> Path {
    if other.is_absolute() {
      ret other.dup()
    }
    mut out := self.dup()
    n := out.wide.len()
    mut last := 0u16
    if n > 0 {
      last = out.wide[n - 1]
    }
    mut first := 0u16
    if !other.wide.is_empty() {
      first = other.wide[0]
    }
    if n > 0 && !is_separator(last) && first != 0 && !is_separator(first) {
      out.wide.push(92u16)
    }
    mut i := 0 as usize
    while i < other.wide.len() {
      out.wide.push(other.wide[i])
      i = i + 1
    }
    ret out
  }

  // The last component, or `None` for a path of nothing but
  // separators or a bare drive prefix.
  pub fn file_name(self: &Self) -> Option<String> {
    mut end := self.wide.len()
    while end > 0 && is_separator(self.wide[end - 1]) {
      end = end - 1
    }
    if end == 0 || is_drive_root(self.wide.as_slice(), end) {
      ret Option::None
    }
    mut start := end
    while start > 0 && !is_separator(self.wide[start - 1]) {
      start = start - 1
    }
    ret unit_to_string(self.wide.as_slice()[start..<end])
  }

  // The path above the last component, or `None` for the root. A
  // component-less relative path answers the empty path.
  pub fn parent(self: &Self) -> Option<Path> {
    mut end := self.wide.len()
    while end > 0 && is_separator(self.wide[end - 1]) {
      end = end - 1
    }
    if end == 0 || is_drive_root(self.wide.as_slice(), end) {
      ret Option::None
    }
    mut start := end
    while start > 0 && !is_separator(self.wide[start - 1]) {
      start = start - 1
    }
    mut units := Vec::<u16>::from_slice(self.wide.as_slice()[0..<start])
    mut keep := units.len()
    while keep > 1 && is_separator(units[keep - 1]) {
      keep = keep - 1
    }
    while units.len() > keep {
      _ := units.pop()
    }
    ret Option::Some(Path { wide: units })
  }

  // The extension after the last dot in the last component, or
  // `None` when the component has none or opens with one.
  pub fn extension(self: &Self) -> Option<String> {
    mut end := self.wide.len()
    while end > 0 && is_separator(self.wide[end - 1]) {
      end = end - 1
    }
    if end == 0 {
      ret Option::None
    }
    mut start := end
    while start > 0 && !is_separator(self.wide[start - 1]) {
      start = start - 1
    }
    mut dot := 0 as usize
    mut found := false
    mut i := start
    while i < end {
      if self.wide[i] == 46u16 {
        found = true
        dot = i
      }
      i = i + 1
    }
    if !found {
      ret Option::None
    }
    if dot == start {
      ret Option::None
    }
    ret unit_to_string(self.wide.as_slice()[(dot + 1)..<end])
  }

  // Whether this path starts with the other, component by component.
  pub fn starts_with(self: &Self, base: &Path) -> bool {
    mine := self.segments()
    theirs := base.segments()
    if theirs.len() > mine.len() {
      ret false
    }
    mut i := 0 as usize
    while i < theirs.len() {
      a := mine.at(i).or_panic_with_message("the component is in the path")
      b := theirs.at(i).or_panic_with_message("the component is in the base")
      if !unit_eq(a.as_slice(), b.as_slice()) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether this path ends with the other, component by component.
  pub fn ends_with(self: &Self, base: &Path) -> bool {
    mine := self.segments()
    theirs := base.segments()
    if theirs.len() > mine.len() {
      ret false
    }
    mut offset := mine.len() - theirs.len()
    mut i := 0 as usize
    while i < theirs.len() {
      a := mine.at(offset + i).or_panic_with_message("the component is in the path")
      b := theirs.at(i).or_panic_with_message("the component is in the base")
      if !unit_eq(a.as_slice(), b.as_slice()) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  fn dup(self: &Self) -> Path {
    ret Path { wide: Vec::<u16>::from_slice(self.as_wide()) }
  }

  // The components, separators removed. Both separator spellings are
  // accepted, as the Windows API does.
  fn segments(self: &Self) -> Vec<Vec<u16>> {
    mut out := Vec::<Vec<u16>>::new()
    mut start := 0 as usize
    mut i := 0 as usize
    while i < self.wide.len() {
      if is_separator(self.wide[i]) {
        if i > start {
          out.push(Vec::<u16>::from_slice(self.wide.as_slice()[start..<i]))
        }
        start = i + 1
      }
      i = i + 1
    }
    if start < self.wide.len() {
      out.push(Vec::<u16>::from_slice(self.wide.as_slice()[start..<self.wide.len()]))
    }
    ret out
  }
}

#[when(target.os == "windows")]
fn is_separator(unit: u16) -> bool {
  ret unit == 47u16 || unit == 92u16
}

// What is left of `C:\` once its trailing separators are gone: the
// letter and colon name a current directory, not a component.
#[when(target.os == "windows")]
fn is_drive_root(units: &[u16], end: usize) -> bool {
  ret end == 2 && units[1] == 58u16
}

#[when(target.os == "windows")]
fn unit_eq(a: &[u16], b: &[u16]) -> bool {
  if a.len() != b.len() {
    ret false
  }
  mut i := 0 as usize
  while i < a.len() {
    if a[i] != b[i] {
      ret false
    }
    i = i + 1
  }
  ret true
}

#[when(target.os == "windows")]
fn unit_to_string(units: &[u16]) -> Option<String> {
  match decode(units) {
    Option::Some(bytes) => {
      ret String::from_slice(bytes.as_slice())
    }
    Option::None => {
      ret Option::None
    }
  }
}

#[when(target.os == "windows")]
fn encode(bytes: &[u8]) -> Vec<u16> {
  mut out := Vec::<u16>::new()
  mut i := 0 as usize
  while i < bytes.len() {
    b0 := bytes[i]
    mut cp := 0u32
    mut width := 1 as usize
    if b0 < 0x80u8 {
      cp = b0 as u32
    } else if b0 < 0xE0u8 {
      cp = ((b0 as u32 & 0x1Fu32) << 6) | (bytes[i + 1] as u32 & 0x3Fu32)
      width = 2
    } else if b0 < 0xF0u8 {
      cp =
        ((b0 as u32 & 0x0Fu32) << 12) |
        ((bytes[i + 1] as u32 & 0x3Fu32) << 6) |
        (bytes[i + 2] as u32 & 0x3Fu32)
      width = 3
    } else {
      cp =
        ((b0 as u32 & 0x07u32) << 18) |
        ((bytes[i + 1] as u32 & 0x3Fu32) << 12) |
        ((bytes[i + 2] as u32 & 0x3Fu32) << 6) |
        (bytes[i + 3] as u32 & 0x3Fu32)
      width = 4
    }
    i = i + width
    if cp <= 0xFFFFu32 {
      out.push(cp as u16)
    } else {
      v := cp - 0x10000u32
      out.push((0xD800u32 + (v >> 10)) as u16)
      out.push((0xDC00u32 + (v & 0x3FFu32)) as u16)
    }
  }
  ret out
}

#[when(target.os == "windows")]
fn decode(units: &[u16]) -> Option<Vec<u8>> {
  mut out := Vec::<u8>::new()
  mut i := 0 as usize
  while i < units.len() {
    u := units[i] as u32
    i = i + 1
    mut cp := 0u32
    if u >= 0xD800u32 && u <= 0xDBFFu32 {
      if i >= units.len() {
        ret Option::None
      }
      lo := units[i] as u32
      if lo < 0xDC00u32 || lo > 0xDFFFu32 {
        ret Option::None
      }
      cp = 0x10000u32 + (((u - 0xD800u32) << 10) | (lo - 0xDC00u32))
      i = i + 1
    } else if u >= 0xDC00u32 && u <= 0xDFFFu32 {
      ret Option::None
    } else {
      cp = u
    }
    if cp < 0x80u32 {
      out.push(cp as u8)
    } else if cp < 0x800u32 {
      out.push((0xC0u32 | (cp >> 6)) as u8)
      out.push((0x80u32 | (cp & 0x3Fu32)) as u8)
    } else if cp < 0x10000u32 {
      out.push((0xE0u32 | (cp >> 12)) as u8)
      out.push((0x80u32 | ((cp >> 6) & 0x3Fu32)) as u8)
      out.push((0x80u32 | (cp & 0x3Fu32)) as u8)
    } else {
      out.push((0xF0u32 | (cp >> 18)) as u8)
      out.push((0x80u32 | ((cp >> 12) & 0x3Fu32)) as u8)
      out.push((0x80u32 | ((cp >> 6) & 0x3Fu32)) as u8)
      out.push((0x80u32 | (cp & 0x3Fu32)) as u8)
    }
  }
  ret Option::Some(out)
}

#[when(testing)] {
  #[when(target.os == "windows")]
  #[test] fn from_str_round_trips_through_to_str() {
    p := Path::from_str("C:\\Users\\alcy")
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "C:\\Users\\alcy", "the text comes back out")
      }
      Option::None => {
        panic("a UTF-8 path hands back its text")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn an_astral_scalar_survives_the_round_trip() {
    p := Path::from_str("a 𝐁 b")
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "a 𝐁 b", "astral text round-trips")
      }
      Option::None => {
        panic("an astral scalar is one unit of UTF-16 surrogate pair, and the pair decodes")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn from_bytes_refuses_bytes_that_are_not_utf8() {
    bad := [0xffu8, 0xfeu8]
    assert(Path::from_bytes(bad.as_slice()).is_none(), "not scalar-shaped bytes are refused")
  }

  #[when(target.os == "windows")]
  #[test] fn a_drive_root_is_absolute() {
    assert(Path::from_str("C:\\").is_absolute(), "a drive prefix is a root")
    assert(Path::from_str("\\\\server\\share").is_absolute(), "a UNC prefix is a root")
    assert(!Path::from_str("C:relative").is_absolute(), "a drive-relative path is not absolute")
  }

  #[when(target.os == "windows")]
  #[test] fn join_puts_one_separator_between_two_components() {
    p := Path::from_str("C:\\Users").join(&Path::from_str("alcy"))
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "C:\\Users\\alcy", "one separator between the components")
      }
      Option::None => {
        panic("the joined path is UTF-8")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn join_prefers_an_absolute_argument() {
    p := Path::from_str("C:\\Users").join(&Path::from_str("D:\\data"))
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "D:\\data", "an absolute component replaces the path")
      }
      Option::None => {
        panic("the argument is UTF-8")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn file_name_strips_trailing_separators() {
    p := Path::from_str("C:\\Users\\alcy\\")
    match p.file_name() {
      Option::Some(name) => {
        assert(name.as_str() == "alcy", "the last component is the answer")
      }
      Option::None => {
        panic("the trailing separators fall off")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn file_name_of_a_root_is_none() {
    assert(Path::from_str("C:\\").file_name().is_none(), "a root has no last component")
  }

  #[when(target.os == "windows")]
  #[test] fn parent_climbs_one_component() {
    match Path::from_str("C:\\Users\\alcy").parent() {
      Option::Some(p) => {
        match p.to_str() {
          Option::Some(text) => {
            assert(text.as_str() == "C:\\Users", "one component fell off")
          }
          Option::None => {
            panic("the parent is UTF-8")
          }
        }
      }
      Option::None => {
        panic("a component path has a parent")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn extension_reads_the_suffix() {
    match Path::from_str("C:\\Users\\alcy\\tool.exe").extension() {
      Option::Some(ext) => {
        assert(ext.as_str() == "exe", "the suffix after the last dot")
      }
      Option::None => {
        panic("a dotted file has an extension")
      }
    }
  }

  #[when(target.os == "windows")]
  #[test] fn starts_with_matches_components_not_bytes() {
    assert(
      Path::from_str("C:\\Users\\alcy").starts_with(&Path::from_str("C:\\Users")),
      "the prefix is a component",
    )
    assert(
      !Path::from_str("C:\\Users\\alcy").starts_with(&Path::from_str("C:\\Use")),
      "a byte prefix is not a component",
    )
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
