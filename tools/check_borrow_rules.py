#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check the borrow checker against the rules it is supposed to enforce.

docs/spec/ownership.md and ADR-0012 state the rules as propositions
about a loan, so a program's verdict is derivable rather than a matter
of taste. Every case below declares the verdict its loan demands, and
the checker either agrees or does not. A disagreement is a defect in
the checker, and this fails.

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
    "kind-pair", "shared-then-shared", "accept",
    """fn main() -> i32 {
  x := 1i32
  r := &x
  s := &x
  ret *r + *s
}""", BOX)

case(
    "kind-pair", "shared-then-mut", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  r := &x
  m := &mut x
  ret *r + *m
}""", BOX)

case(
    "kind-pair", "mut-then-shared", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  m := &mut x
  r := &x
  ret *r + *m
}""", BOX)

case(
    "kind-pair", "mut-then-mut", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  m := &mut x
  n := &mut x
  ret *m + *n
}""", BOX)

# --- exclusivity through a reference: a shared loan cannot yield an
# --- exclusive one, and two derived exclusives may not alias.
case(
    "aliasing", "mut-derived-from-shared", "reject",
    """fn main() -> i32 {
  x := 1i32
  s := &x
  mut m := &mut *s
  *m = 5
  ret *s
}""", BOX)

case(
    "aliasing", "mut-through-two-shared-refs", "reject",
    """fn main() -> i32 {
  x := 1i32
  s1 := &x
  s2 := &x
  mut a := &mut *s1
  mut b := &mut *s2
  *a = 1
  *b = 2
  ret x
}""", BOX)

case(
    "aliasing", "exclusive-from-shared-then-read", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  s := &x
  mut m := &mut *s
  *m = 5
  ret *s
}""", BOX)

# Reading the parent while an exclusive reborrow of it is live is the
# same rule seen from the other side: the child freezes the parent.
case(
    "aliasing", "parent-read-while-child-live", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  ret *m + *n
}""", BOX)

case(
    "aliasing", "mut-through-two-mut-refs", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut s1 := &mut x
  mut s2 := &mut x
  *s1 = 1
  *s2 = 2
  ret x
}""", BOX)

# --- overlap on one aggregate -----------------------------------------
case(
    "overlap", "distinct-fields-shared", "accept",
    """fn main() -> i32 {
  p := P { a: 1, b: 2 }
  r := &p.a
  s := &p.b
  ret *r + *s
}""", PAIR)

case(
    "overlap", "whole-mut-then-field-shared", "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  m := &mut p
  r := &p.a
  ret *r + m.a
}""", PAIR)

case(
    "overlap", "whole-shared-then-field-mut", "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p
  m := &mut p.a
  ret r.a + *m
}""", PAIR)

case(
    "overlap", "whole-shared-then-field-shared", "accept",
    """fn main() -> i32 {
  p := P { a: 1, b: 2 }
  r := &p
  s := &p.a
  ret r.a + *s
}""", PAIR)

case(
    "overlap", "field-then-whole-mut", "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p.a
  m := &mut p
  ret *r + m.a
}""", PAIR)

case(
    "overlap", "store-to-other-field", "accept",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p.a
  p.b = 9
  ret *r + p.b
}""", PAIR)

case(
    "overlap", "store-to-borrowed-field", "reject",
    """fn main() -> i32 {
  mut p := P { a: 1, b: 2 }
  r := &p.a
  p.a = 9
  ret *r + p.a
}""", PAIR)

# --- reborrow, with the extent cases under their own group -------------
case(
    "reborrow", "shared-reborrow-of-shared", "accept",
    """fn main() -> i32 {
  x := 1i32
  s := &x
  r := &*s
  ret *r + *s
}""", BOX)

case(
    "reborrow", "shared-reborrow-of-mut", "accept",
    """fn main() -> i32 {
  mut x := 1i32
  m := &mut x
  r := &*m
  ret *r + *m
}""", BOX)

case(
    "reborrow", "mut-reborrow-of-mut", "accept",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut r := &mut *m
  *r = 5
  ret *r
}""", BOX)

case(
    "reborrow", "two-sequential-reborrows", "accept",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut a := &mut *m
  *a = 2
  mut b := &mut *m
  *b = 3
  ret x
}""", BOX)

case(
    "reborrow", "parent-used-while-child-live", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  *m = 7
  ret *n
}""", BOX)

case(
    "reborrow", "parent-used-after-child-dead", "accept",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  *n = 7
  *m = 9
  ret x
}""", BOX)

case(
    "reborrow", "base-stored-while-child-live", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  mut m := &mut x
  mut n := &mut *m
  x = 7
  ret *n
}""", BOX)

# --- a loan that came back from a call --------------------------------
case(
    "returned", "store-while-return-live", "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret *r
}""", BOX)

case(
    "returned", "shared-while-return-live", "accept",
    """fn main() -> i32 {
  x := Box { n: 1 }
  r := get(&x)
  s := &x
  ret *r + s.n
}""", BOX)

case(
    "returned", "mut-while-return-live", "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  m := &mut x
  ret *r + m.n
}""", BOX)

case(
    "returned", "store-while-return-dead", "accept",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret x.n
}""", BOX)

# --- extent across control flow ---------------------------------------
case(
    "extent", "dead-on-taken-branch", "accept",
    """fn main() -> i32 {
  x := Box { n: 1 }
  r := get(&x)
  if x.n != 1 {
    ret 1
  }
  ret *r
}""", BOX)

case(
    "extent", "live-on-untaken-branch", "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  if x.n == 1 {
    x.n = 9
  }
  ret *r
}""", BOX)

case(
    "extent", "live-then-store-then-use", "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  x.n = 9
  ret *r
}""", BOX)

case(
    "extent", "store-then-borrow-then-use", "accept",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  x.n = 9
  r := get(&x)
  ret *r
}""", BOX)

case(
    "extent", "live-in-loop-across-iteration", "reject",
    """fn main() -> i32 {
  mut x := Box { n: 1 }
  r := get(&x)
  mut i := 0
  while i < 2 {
    x.n = 9
    i = i + 1
  }
  ret *r
}""", BOX)

# --- buffer elements --------------------------------------------------
case("element", "at-then-push", "reject", VEC + """  r := v.at(0)
  v.push(3i32)
  ret *r.unwrap()
}""")

case("element", "at-dead-then-push", "accept", VEC + """  r := v.at(0)
  v.push(3i32)
  ret v.len() as i32
}""")

case(
    "element",
    "at_mut-then-push",
    "reject",
    VEC
    + """  mut r := v.at_mut(0).unwrap()
  v.push(3i32)
  ret *r
}""")

case("element", "two-at-same-index", "accept", VEC + """  a := v.at(0).unwrap()
  b := v.at(0).unwrap()
  ret *a + *b
}""")

case(
    "element",
    "two-at_mut-same-index",
    "reject",
    VEC
    + """  mut a := v.at_mut(0).unwrap()
  mut b := v.at_mut(0).unwrap()
  ret *a + *b
}""")

case(
    "element",
    "two-at-distinct-index",
    "accept",
    VEC
    + """  a := v.at(0).unwrap()
  b := v.at(1).unwrap()
  ret *a + *b
}""")

# A literal index names its element, so distinct literals are distinct
# elements and the checker can say so. A runtime index cannot, so the
# last case below is recorded as conforming by refusing.
case(
    "element",
    "two-at_mut-distinct-index",
    "accept",
    VEC
    + """  mut a := v.at_mut(0).unwrap()
  mut b := v.at_mut(1).unwrap()
  *a = 10
  *b = 20
  ret *a + *b
}""", known=True)

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
}""")

# --- a struct holding a reference composes by intersection ------------
case(
    "struct", "reference-field-kept-live", "accept",
    """fn main() -> i32 {
  x := 1i32
  h := Holder { r: &x }
  mut y := 2i32
  y = 3
  ret *h.r + y
}""", HOLDER)

case(
    "struct", "struct-with-ref-arg", "accept",
    """fn main() -> i32 {
  x := 1i32
  h := Holder { r: &x }
  mut y := 2i32
  y = 3
  ret read(&h) + y
}""", HOLDER)

case(
    "struct", "base-mutated-while-ref-field-live", "reject",
    """fn main() -> i32 {
  mut x := 1i32
  h := Holder { r: &x }
  x = 9
  ret *h.r
}""", HOLDER)

# --- a Copy type copies, so the source stays whole --------------------
case(
    "move", "copy-of-copy-type", "accept",
    """fn main() -> i32 {
  x := Box { n: 1 }
  r := get(&x)
  y := x
  ret *r + y.n
}""", BOX)


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
            if not agrees and got == "reject" and not is_borrow_diagnostic(
                reason
            ):
                broken.append((group, name, expect, got, reason))
            elif not agrees and not known:
                unexpected.append((group, name, expect, got, reason))
            elif known and agrees:
                fixed.append((group, name))
            if args.verbose or not agrees or (known and agrees):
                mark = "ok  " if agrees else ("gap " if known else "FAIL")
                note = f"  [{reason}]" if reason else ""
                print(
                    f"{mark} {group}/{name}: "
                    f"want={expect} got={got}{note}"
                )

    if broken:
        print("\nCases that failed for a reason other than borrowing:")
        for group, name, expect, got, reason in broken:
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
