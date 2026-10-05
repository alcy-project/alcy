#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check the borrow checker against the rules it is supposed to enforce.

docs/spec/ownership.md and `docs/adr/0012-reborrow-on-reference-read.md` state the
rules as propositions about a loan, so a program's verdict is derivable
rather than a matter of taste. Every case below declares the verdict its
loan demands, and the checker either agrees or does not. A disagreement
is a defect in the checker, and this fails.

Two disciplines keep the cases honest, because a case that does not
test borrowing cannot be evidence about borrowing:

  - Every main returns i32 and ends in `ret` of an i32 expression, so
    nothing in a case can fail for a reason other than borrowing. A
    rejection whose diagnostic is not a borrow diagnostic is reported
    as a broken case, not as a pass.
  - A case expecting a rejection names how its loan is still live
    afterwards. Without that, the loan simply dies at the point under
    test and the acceptance is correct rather than a hole.

A case may be recorded as a known gap: the rule is stated, the checker
does not meet it, and the gap is tracked here so it is neither
forgotten nor mistaken for agreement. Known gaps do not fail the run;
any deviation that is not a known gap does. When a fix makes a known
gap conform, the run says so, and the entry can become a plain
expectation.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

from utils.env import run_environment
from utils.paths import project_root_dir

BOX = "struct Box { n: i32 }\nfn get(b: &Box) -> &i32 { ret &b.n }\n"
PAIR = "struct P { a: i32, b: i32 }\n"
HOLDER = "struct Holder { r: &i32 }\nfn read(h: &Holder) -> i32 { ret *h.r }\n"
VEC = "fn main() -> i32 {\n"
VEC += "  mut v := Vec::<i32>::new()\n"
VEC += "  v.push(1i32)\n  v.push(2i32)\n"

# (group, name, expected, known_gap, source)
CASES = []


def case(group, name, expect, body, prelude="", known=False):
    CASES.append((group, name, expect, known, prelude + body))


# --- the four loan kinds against one place ----------------------------
case(
    "kind-pair",
    "shared-then-shared",
    "accept",
    """fn main() -> i32 {
  x := 1i32
  r := &x
  s := &x
  ret *r + *s
}""",
    BOX,
)

case(
    "kind-pair",
    "shared-then-mut",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  r := &x
  m := &mut x
  ret *r + *m
}""",
    BOX,
)

case(
    "kind-pair",
    "mut-then-shared",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  m := &mut x
  r := &x
  ret *r + *m
}""",
    BOX,
)

case(
    "kind-pair",
    "mut-then-mut",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  m := &mut x
  n := &mut x
  ret *m + *n
}""",
    BOX,
)

# --- exclusivity through a reference: a shared loan cannot yield an
# --- exclusive one, and two derived exclusives may not alias.
case(
    "aliasing",
    "mut-derived-from-shared",
    "reject",
    """fn main() -> i32 {
  x := 1i32
  s := &x
  mut m := &mut *s
  *m = 5
  ret *s
}""",
    BOX,
)

case(
    "aliasing",
    "mut-through-two-shared-refs",
    "reject",
    """fn main() -> i32 {
  x := 1i32
  s1 := &x
  s2 := &x
  mut a := &mut *s1
  mut b := &mut *s2
  *a = 1
  *b = 2
  ret x
}""",
    BOX,
)

case(
    "aliasing",
    "exclusive-from-shared-then-read",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  s := &x
  mut m := &mut *s
  *m = 5
  ret *s
}""",
    BOX,
)

# Reading the parent while an exclusive reborrow of it is live is the
# same rule seen from the other side: the child freezes the parent.
case(
    "aliasing",
    "parent-read-while-child-live",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  ret *m + *n
}""",
    BOX,
)

case(
    "aliasing",
    "mut-through-two-mut-refs",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut s1 := &mut x
  mut s2 := &mut x
  *s1 = 1
  *s2 = 2
  ret x
}""",
    BOX,
)

# --- overlap on one aggregate -----------------------------------------
case(
    "overlap",
    "distinct-fields-shared",
    "accept",
    """fn main() -> i32 {
  p := P { a: 1, b: 2 }
  r := &p.a
  s := &p.b
  ret *r + *s
}""",
    PAIR,
)

case(
    "overlap",
    "whole-mut-then-field-shared",
    "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  m := &mut p
  r := &p.a
  ret *r + m.a
}""",
    PAIR,
)

case(
    "overlap",
    "whole-shared-then-field-mut",
    "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p
  m := &mut p.a
  ret r.a + *m
}""",
    PAIR,
)

case(
    "overlap",
    "whole-shared-then-field-shared",
    "accept",
    """fn main() -> i32 {
  p := P { a: 1, b: 2 }
  r := &p
  s := &p.a
  ret r.a + *s
}""",
    PAIR,
)

case(
    "overlap",
    "field-then-whole-mut",
    "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p.a
  m := &mut p
  ret *r + m.a
}""",
    PAIR,
)

case(
    "overlap",
    "store-to-other-field",
    "accept",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p.a
  p.b = 9
  ret *r + p.b
}""",
    PAIR,
)

case(
    "overlap",
    "store-to-borrowed-field",
    "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p.a
  p.a = 9
  ret *r + p.a
}""",
    PAIR,
)

# --- reborrow, with the extent cases under their own group -------------
case(
    "reborrow",
    "shared-reborrow-of-shared",
    "accept",
    """fn main() -> i32 {
  x := 1i32
  s := &x
  r := &*s
  ret *r + *s
}""",
    BOX,
)

case(
    "reborrow",
    "shared-reborrow-of-mut",
    "accept",
    """fn main() -> i32 {
  mut x := 1i32
  m := &mut x
  r := &*m
  ret *r + *m
}""",
    BOX,
)

case(
    "reborrow",
    "mut-reborrow-of-mut",
    "accept",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut r := &mut *m
  *r = 5
  ret *r
}""",
    BOX,
)

case(
    "reborrow",
    "two-sequential-reborrows",
    "accept",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut a := &mut *m
  *a = 2
  mut b := &mut *m
  *b = 3
  ret x
}""",
    BOX,
)

case(
    "reborrow",
    "parent-used-while-child-live",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  *m = 7
  ret *n
}""",
    BOX,
)

case(
    "reborrow",
    "parent-used-after-child-dead",
    "accept",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  *n = 7
  *m = 9
  ret x
}""",
    BOX,
)

case(
    "reborrow",
    "base-stored-while-child-live",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  x = 7
  ret *n
}""",
    BOX,
)

# --- a loan that came back from a call --------------------------------
case(
    "returned",
    "store-while-return-live",
    "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret *r
}""",
    BOX,
)

case(
    "returned",
    "shared-while-return-live",
    "accept",
    """fn main() -> i32 {
  x := Box { n: 1 }
  r := get(&x)
  s := &x
  ret *r + s.n
}""",
    BOX,
)

case(
    "returned",
    "mut-while-return-live",
    "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  m := &mut x
  ret *r + m.n
}""",
    BOX,
)

case(
    "returned",
    "store-while-return-dead",
    "accept",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret x.n
}""",
    BOX,
)

# --- extent across control flow ---------------------------------------
case(
    "extent",
    "dead-on-taken-branch",
    "accept",
    """fn main() -> i32 {
  x := Box { n: 1 }
  r := get(&x)
  if x.n != 1 {
    ret 1
  }
  ret *r
}""",
    BOX,
)

case(
    "extent",
    "live-on-untaken-branch",
    "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  if x.n == 1 {
    x.n = 9
  }
  ret *r
}""",
    BOX,
)

case(
    "extent",
    "live-then-store-then-use",
    "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret *r
}""",
    BOX,
)

case(
    "extent",
    "store-then-borrow-then-use",
    "accept",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  x.n = 9
  r := get(&x)
  ret *r
}""",
    BOX,
)

case(
    "extent",
    "live-in-loop-across-iteration",
    "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  mut i := 0
  while i < 2 {
    x.n = 9
    i = i + 1
  }
  ret *r
}""",
    BOX,
)

# --- buffer elements --------------------------------------------------
case(
    "element",
    "at-then-push",
    "reject",
    VEC
    + """  r := v.at(0)
  v.push(3i32)
  ret *r.unwrap()
}""",
)

case(
    "element",
    "at-dead-then-push",
    "accept",
    VEC
    + """  r := v.at(0)
  v.push(3i32)
  ret v.len() as i32
}""",
)

case(
    "element",
    "at_mut-then-push",
    "reject",
    VEC
    + """  mut r := v.at_mut(0).unwrap()
  v.push(3i32)
  ret *r
}""",
)

case(
    "element",
    "two-at-same-index",
    "accept",
    VEC
    + """  a := v.at(0).unwrap()
  b := v.at(0).unwrap()
  ret *a + *b
}""",
)

case(
    "element",
    "two-at_mut-same-index",
    "reject",
    VEC
    + """  mut a := v.at_mut(0).unwrap()
  mut b := v.at_mut(0).unwrap()
  ret *a + *b
}""",
)

case(
    "element",
    "two-at-distinct-index",
    "accept",
    VEC
    + """  a := v.at(0).unwrap()
  b := v.at(1).unwrap()
  ret *a + *b
}""",
)

# `at_mut` takes `&mut Self`, so the receiver is what governs two calls,
# not the index. Holding one result across a second call is two
# exclusive loans of the vector at once, which the rules forbid whatever
# the indices are; the index is invisible across the call anyway, since
# the accessor reads it as a parameter.
case(
    "element",
    "two-at_mut-both-live",
    "reject",
    VEC
    + """  mut a := v.at_mut(0).unwrap()
  mut b := v.at_mut(1).unwrap()
  *a = 10
  *b = 20
  ret *a + *b
}""",
)

case(
    "element",
    "two-at_mut-first-dead",
    "accept",
    VEC
    + """  mut a := v.at_mut(0).unwrap()
  *a = 10
  mut b := v.at_mut(1).unwrap()
  *b = 20
  ret 0
}""",
)

case(
    "element",
    "two-at_mut-runtime-index",
    "reject",
    VEC
    + """  mut i := 0 as usize
  mut a := v.at_mut(i).unwrap()
  i = 1
  mut b := v.at_mut(i).unwrap()
  ret *a + *b
}""",
)

# --- map entries: a view from `get` is a loan of the map, and a call
# --- that mutates the map conflicts with it ---------------------------
case(
    "map",
    "view-then-insert",
    "reject",
    """fn main() -> i32 {
  mut m := Map::<i32>::new()
  _ := m.insert("a", 1i32)
  view := m.get("a")
  _ := m.insert("b", 2i32)
  ret *view.unwrap()
}""",
)

case(
    "map",
    "view-then-len",
    "accept",
    """fn main() -> i32 {
  mut m := Map::<i32>::new()
  _ := m.insert("a", 1i32)
  view := m.get("a")
  n := m.len()
  ret *view.unwrap() + n as i32 - 1
}""",
)

case(
    "map",
    "view-then-remove",
    "reject",
    """fn main() -> i32 {
  mut m := Map::<i32>::new()
  _ := m.insert("a", 1i32)
  view := m.get("a")
  _ := m.remove("a")
  ret *view.unwrap()
}""",
)

# A method receiver names a field, and the loan covers that field; a
# view into one field leaves another field free to mutate.
case(
    "map",
    "view-then-other-field",
    "accept",
    """struct Both { a: Map<i32>, b: Map<i32> }
fn main() -> i32 {
  mut x := Both { a: Map::<i32>::new(), b: Map::<i32>::new() }
  _ := x.a.insert("k", 5i32)
  view := x.a.get("k")
  _ := x.b.insert("j", 7i32)
  ret *view.unwrap() - 5
}""",
)

case(
    "map",
    "field-view-then-mutate",
    "reject",
    """struct Both { a: Map<i32>, b: Map<i32> }
fn main() -> i32 {
  mut x := Both { a: Map::<i32>::new(), b: Map::<i32>::new() }
  _ := x.a.insert("k", 5i32)
  view := x.a.get("k")
  _ := x.a.insert("j", 7i32)
  ret *view.unwrap() - 5
}""",
)

# --- projection through a call: the summary names the field, so a loan
# --- into one field does not cover the whole argument ------------------
case(
    "projection",
    "mut-field-then-other-field",
    "accept",
    """fn main() -> i32 {
  mut q := P { a: 1, b: 2 }
  mut r := mget_a(&mut q)
  *r = 5
  q.b = 9
  ret *r + q.b
}""",
    PAIR + "fn mget_a(p: &mut P) -> &mut i32 {\n  ret &mut p.a\n}\n",
)

case(
    "projection",
    "mut-field-then-same-field",
    "reject",
    """fn main() -> i32 {
  mut q := P { a: 1, b: 2 }
  mut r := mget_a(&mut q)
  *r = 5
  q.a = 9
  ret *r
}""",
    PAIR + "fn mget_a(p: &mut P) -> &mut i32 {\n  ret &mut p.a\n}\n",
)

case(
    "projection",
    "shared-field-then-other-field",
    "accept",
    """fn main() -> i32 {
  mut q := P { a: 1, b: 2 }
  r := get_a(&q)
  q.b = 9
  ret *r + q.b
}""",
    PAIR + "fn get_a(p: &P) -> &i32 {\n  ret &p.a\n}\n",
)

# --- a str view into a string is a loan of the string -----------------
case(
    "view",
    "as_str-then-push",
    "reject",
    """fn main() -> i32 {
  mut s := String::new()
  s.push(104u8)
  r := s.as_str()
  s.push(105u8)
  print(r)
  ret 0
}""",
)

case(
    "view",
    "as_str-copy-then-push",
    "reject",
    """fn main() -> i32 {
  mut s := String::new()
  s.push(104u8)
  r := s.as_str()
  t := r
  s.push(105u8)
  print(t)
  ret 0
}""",
)

case(
    "view",
    "as_str-dead-then-push",
    "accept",
    """fn main() -> i32 {
  mut s := String::new()
  s.push(104u8)
  r := s.as_str()
  s.push(105u8)
  ret s.len() as i32
}""",
)

case(
    "view",
    "push-then-view",
    "accept",
    """fn main() -> i32 {
  mut s := String::new()
  s.push(104u8)
  s.push(105u8)
  print(s.as_str())
  ret 0
}""",
)

# --- a slice is a str in the shape of any element: a view that
# --- carries the buffer's loan, with the length half owning nothing.
case(
    "view",
    "as_slice-then-push",
    "reject",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  s := v.as_slice()
  v.push(2i32)
  ret slice_len(s) as i32
}""",
)

case(
    "view",
    "as_slice-copy-then-push",
    "reject",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  s := v.as_slice()
  t := s
  v.push(2i32)
  ret slice_len(t) as i32
}""",
)

case(
    "view",
    "as_slice-last-use-then-push",
    "accept",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  s := v.as_slice()
  n := slice_len(s)
  v.push(2i32)
  ret n as i32
}""",
)

case(
    "view",
    "as_mut_slice-write-then-push",
    "reject",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  mut s := v.as_mut_slice()
  s[0] = 9
  v.push(2i32)
  ret slice_len(s) as i32
}""",
)

case(
    "view",
    "as_mut_slice-last-use-then-push",
    "accept",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  mut s := v.as_mut_slice()
  s[0] = 9
  n := slice_len(s)
  v.push(2i32)
  ret *v.at(0).unwrap() + n as i32 - 10
}""",
)

case(
    "view",
    "shared-slice-then-mut-slice",
    "reject",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  s := v.as_slice()
  m := v.as_mut_slice()
  ret slice_len(s) + slice_len(m)
}""",
)

case(
    "view",
    "mut-slice-element-write",
    "accept",
    """fn touch(mut v: &mut [i32]) {
  v[1] = 9
}
fn main() -> i32 {
  mut a := [1i32, 2i32]
  touch(&mut a)
  ret a[1] - 9
}""",
)

case(
    "view",
    "slice-element-then-push",
    "reject",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(2i32)
  s := v.as_slice()
  r := &s[1]
  v.push(3i32)
  ret *r
}""",
)

case(
    "view",
    "slice-element-copy-then-push",
    "accept",
    """fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  v.push(2i32)
  s := v.as_slice()
  r := &s[1]
  n := *r
  v.push(3i32)
  ret n - 2
}""",
)

# --- a run borrows the whole container: the view carries the loan
# --- the borrow took, so writing the container behind a live run
# --- conflicts, and re-slicing extends the loan rather than cutting it.
case(
    "subslice",
    "mut-run-write-then-array-write",
    "reject",
    """fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  mut m := &mut a[0..<2]
  m[0] = 9
  a[2] = 8
  ret m[0] + a[2] - 17
}""",
)

case(
    "subslice",
    "mut-run-last-use-then-array-write",
    "accept",
    """fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  mut m := &mut a[0..<2]
  m[0] = 9
  n := m[0]
  a[2] = 8
  ret n + a[2] - 17
}""",
)

case(
    "subslice",
    "shared-run-then-mut-run",
    "reject",
    """fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  s := &a[0..<2]
  mut m := &mut a[1..<3]
  ret s[0] + m[0] - 3
}""",
)

case(
    "subslice",
    "reslice-then-array-write",
    "reject",
    """fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  s := &a[..]
  t := s[0..<2]
  a[0] = 9
  ret t[0] + a[0] - 10
}""",
)

case(
    "subslice",
    "reslice-last-use-then-array-write",
    "accept",
    """fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  s := &a[..]
  t := s[0..<2]
  n := t[0]
  a[0] = 9
  ret n + a[0] - 10
}""",
)

# A `str` run should carry the buffer's loan like a slice run does,
# but views derived through `ExtractValue` on a `str` drop it: the
# `str_slice` intrinsic has the same hole today, so this is a known
# gap in the checker rather than in the run itself.
case(
    "subslice",
    "str-run-then-buffer-write",
    "reject",
    """fn main() -> i32 {
  mut s := String::new()
  s.push(104u8)
  s.push(105u8)
  t := s.as_str()[0..<1]
  s.push(106u8)
  ret str_len(t) as i32
}""",
    known=True,
)

# --- a struct holding a reference composes by intersection ------------
case(
    "struct",
    "reference-field-kept-live",
    "accept",
    """fn main() -> i32 {
  x := 1i32
  h := Holder { r: &x }
  mut y := 2i32
  y = 3
  ret *h.r + y
}""",
    HOLDER,
)

case(
    "struct",
    "struct-with-ref-arg",
    "accept",
    """fn main() -> i32 {
  x := 1i32
  h := Holder { r: &x }
  mut y := 2i32
  y = 3
  ret read(&h) + y
}""",
    HOLDER,
)

case(
    "struct",
    "base-mutated-while-ref-field-live",
    "reject",
    """fn main() -> i32 {
  mut x := 1i32
  h := Holder { r: &x }
  x = 9
  ret *h.r
}""",
    HOLDER,
)

# --- a Copy type copies, so the source stays whole --------------------
case(
    "move",
    "copy-of-copy-type",
    "accept",
    """fn main() -> i32 {
  x := Box { n: 1 }
  r := get(&x)
  y := x
  ret *r + y.n
}""",
    BOX,
)


def is_borrow_diagnostic(reason):
    """Whether a rejection came from the borrow checker.

    The borrow checker's own codes are 6xxx. Anything else means the
    case is broken rather than the checker, which matters because a
    type error would otherwise read as agreement.
    """
    return reason.startswith("E6") or "borrowed" in reason


def run_case(alcy, workdir, case):
    group, name, expect, known, source = case
    path = workdir / f"{group}-{name}.al"
    path.write_text(source + "\n", encoding="utf-8")
    proc = subprocess.run(
        [str(alcy), "check", "--file", str(path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        env=run_environment(),
    )
    got = "reject" if proc.returncode != 0 else "accept"
    out = proc.stdout + proc.stderr
    match = re.search(r"error\[(E\d+)\]: ([^\n]*)", out)
    reason = f"{match.group(1)} {match.group(2).strip()}" if match else ""
    return got, reason


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Check the borrow checker against the ownership rules."
    )
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="List every case, not only the deviations",
    )
    args = parser.parse_args()

    alcy = project_root_dir / "out" / args.build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    if not alcy.is_file():
        print(f"alcy binary not found in out/{args.build_subdir}/")
        return 1

    unexpected = []
    broken = []
    fixed = []
    total = 0
    with tempfile.TemporaryDirectory(prefix="alcy_borrow_sweep_") as tmp:
        workdir = Path(tmp)
        for case in CASES:
            total += 1
            group, name, expect, known, _ = case
            got, reason = run_case(alcy, workdir, case)
            agrees = got == expect
            if not agrees and got == "reject" and not is_borrow_diagnostic(reason):
                broken.append((group, name, expect, got, reason))
            elif not agrees and not known:
                unexpected.append((group, name, expect, got, reason))
            elif known and agrees:
                fixed.append((group, name))
            if args.verbose or not agrees or (known and agrees):
                mark = "ok  " if agrees else ("gap " if known else "FAIL")
                note = f"  [{reason}]" if reason else ""
                print(f"{mark} {group}/{name}: want={expect} got={got}{note}")

    if broken:
        print("\nCases that failed for a reason other than borrowing:")
        for group, name, _expect, _got, reason in broken:
            print(f"  - {group}/{name}: {reason}")
        print("These cases are broken, so they say nothing about the rules.")

    if fixed:
        print("\nKnown gaps that now conform; make them plain expectations:")
        for group, name in fixed:
            print(f"  - {group}/{name}")

    gaps = sum(1 for c in CASES if c[3])
    if unexpected:
        print(f"\nborrrow rules: {len(unexpected)} unexpected deviation(s)")
        for group, name, expect, got, reason in unexpected:
            print(f"  - {group}/{name}: want {expect}, got {got} {reason}")
        return 1
    if broken:
        print("\nborrow rules: sweep cases are broken")
        return 1
    print(
        f"borrow rules: {total} cases conform"
        + (f", {gaps} known gap(s) tracked" if gaps else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
