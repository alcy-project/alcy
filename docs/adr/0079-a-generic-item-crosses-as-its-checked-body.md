# ADR-0079: A generic item crosses as its checked body

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-21

## Context

ADR-0078 recorded the summary a dependent reads and left the generic
half open. A generic item's row was marked as needing a body, the row
was dropped rather than spelled, and a dependent that named one read
"Unresolved import". Three ways past that were named: carry typed
bodies, pre-instantiate a declared set in the producer, or leave the
boundary closed for generics. Two of the three were workarounds. One
asks a service to build the instantiation, which is a remote compiler
with a protocol, not a fetcher, and the other spells the instantiations
the producer happened to build under synthesized names, which is a
second naming scheme for a type the source already names and a set the
producer cannot know, because the dependent's witness types are the
dependent's own.

What actually stands in the way is smaller than that. The summary is
not a header: the `.summary` sidecar a library build leaves is the
whole serialized package, bodies included. The body is already
carried. What the IR cannot express is a type parameter, so a body
cannot be carried *as the body of a generic item*: the type tags have
no parameter, a generic item's shape exists only per instantiation,
and the analyzer re-checks a generic body once per instantiation
(ADR-0063) rather than once against its parameters.

So what is missing is a representation and a pass, not a wall.

## Decision

**A generic item crosses as its declaration and its checked body, and
the dependent instantiates it.**

Three parts, each a decision of its own.

**The analyzer checks a generic body once, against its parameters.** A
declaration's signature is resolved with its type parameters, and the
body is checked against that signature once, in that form. A use
instantiates the checked body; it does not check it again. This
supersedes ADR-0063, whose "checked at its instantiations, and nowhere
else" made the library's own errors surface at a caller's line, and
whose justification (bounds cover the hard case) did not cover its own
example.

**The IR can name a type parameter.** A type node carries the index of
a parameter of the item it belongs to, and a generic item is emitted
once, in that form. A call the parameter's type decides is recorded as
the member the parameter's bounds require, so the specialization pass
resolves it when the argument is known.

**Specialization is a pass over IR.** After linking, the program names
every instantiation it needs, and the pass substitutes and clones
bodies, memoized by (item, type arguments). A dependent's use of a
library generic produces the specialization in the dependent's build.

A body that is monomorphic is still not carried in a summary: the
dependent links the library's object for it. A generic body is carried,
in the parameterized form, because no concrete object can hold it. The
summary's `needs_body` flag goes away with it: a row names a body or it
names nothing.

**One specialization mechanism.** The AST-level instantiation walk is
deleted. In-package and cross-package generics go through the same
pass, and a generic body is checked once in either case.

## Consequences

A dependent can call a library's generic, and a dependent's own generic
can call it, which neither workaround could do: the producer cannot
pre-instantiate a type it has never seen.

Every cross-package generic call is a direct call to a monomorphic
body. It is inlinable, devirtualized by construction, and carries no
table, no box, and no lookup at run time. The specialization happens
in the build, which is where the cost belongs.

A generic body is checked in the library, once, so a mistake is
reported against the library's source rather than a caller's, and the
library can be published with a body it has checked.

The costs are named here rather than discovered later. The IR gains a
form the verifier must accept before specialization and that no backend
ever sees. The specialization pass replaces the AST walk, so the
analyzer's per-instantiation resolution goes with it. The binary
format changes; the project is pre-MVP, so there is no compatibility
to keep.

What this record does not settle, because each is a decision of its
own: where the specialization pass sits relative to the borrow pass, a
generic body being borrowed once or once per specialization; how a call
on a parameter resolves, which is the bounds' question and not the
pass's; and whether a generic body may call a spec method on a
parameter at all.

## Staged landing

**Not landed.** Until these land, a generic body is checked at its
instantiations (ADR-0063), and a dependent still cannot name a generic
item.

1. A type parameter in the IR: a type node that names a parameter of
   the item it belongs to, and a generic item emitted once in that
   form.
2. The analyzer checks a generic body once, against its parameters.
3. A specialization pass over IR, after linking.
4. The summary carries a generic body, and the `needs_body` flag
   goes.
5. The AST-level instantiation walk is deleted.
