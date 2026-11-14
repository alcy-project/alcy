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

Amended at its landing: the pass sits **before the borrow pass**, in
each package's own build. Linking is object-level and a package lowers
to its own IR, so there is no whole-program IR to specialize after
linking; the dependent reads the parametric body from the summary and
emits the instance beside its own code. The borrow pass therefore sees
only monomorphic code, and a generic body is borrowed once per
specialization - which settles what this record left open. The
instantiations the pass resolves are the ones the lowered IR names: a
call the parameter's acts decide is the dispatch record ADR-0080
describes, and a call to a generic item is the item with the type
arguments the definition-site check already resolved, so the pass
substitutes the concrete arguments and clones the body. The generic
item is emitted once, in the parametric form, and a parametric
function is one no backend ever receives.

A body that is monomorphic is still not carried in a summary: the
dependent links the library's object for it. A generic body is carried,
in the parameterized form, because no concrete object can hold it. The
summary's `needs_body` flag narrows to what it was for: a row that
names a carried body carries no flag, and an item whose body cannot
cross keeps a declaration-only row with it (item 5).

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

What this record did not settle, and its landing has: where the
specialization pass sits relative to the borrow pass, and whether a
generic body is borrowed once or once per specialization. How a call
on a parameter resolves is ADR-0080.

One case did not fit the decision as it landed: a generic whose
signature names an owning function type, whose witness was per
instantiation (ADR-0075). ADR-0116 closes it - the closure implements the Fn spec its signature
names, the literal binds the return slot once, and the specialization
derives the concrete impl - and the caveat is the work that record
carries into ADR-0079's second slice.

## Staged landing

**The walk is deleted where the once-check stands in.** A dependent
reads a generic body from a summary and instantiates it, and a call
instantiates a signature rather than walking a body; the cases the
definition-site pass cannot stand in for keep their per-instantiation
check.

1. Landed. A type parameter in the IR: a type node that names a
   parameter of the item it belongs to (`1ed02420`).
2. Landed. The analyzer checks a generic body once, against its
   parameters: a dedicated pass binds each
   declared parameter to the `Param` node its index names, resolves
   the declaration's own signature under it, and checks the body
   against that once. The walk visits every generic item the package's
   own modules declare, instantiated or not, so a mistake in a generic
   nothing
   calls is still reported where it is written. A call the parameter's
   type decides resolves
   through the parameter's declared bounds (ADR-0080), and a call to a
   generic callee resolves only its signature, because each item is
   checked in its own context. The pass's records carry `PARAM_INST`,
   the sentinel the lowering keys on to leave a definition-site body
   for the specialization pass rather than lower it concretely.
   What cannot stand in yet stays per-instantiation: a value parameter,
   a `comp fn`, an owning `fn()` slot (ADR-0116), a parameter bounded
   by the `Fn` spec (`F: Fn<...>`, whose witness is a fact of the
   instantiation), and an intrinsic's shape a bare parameter would
   decide. An unsuffixed numeric literal meeting a parameter adopts it,
   and the instantiation's own check settles the width.
3. Landed. A specialization pass over IR, before the borrow pass, in
   each package's build (the Decision above). The layout lowering would
   otherwise bake into a body landed ahead of the pass: enum accesses
   are semantic opcodes whose operands' types settle the discriminant,
   the niche's spell, and the payload offsets at emission, and a copy's
   alignment is its operand's pointee, which the backend reads. A
   parametric body therefore holds no layout for the substitution to
   touch. The parametric form landed next: lowering emits each
   definition-site body once with the `Param` nodes its signature
   resolved, lists it as a function no pass or backend receives, and
   records what it cannot name - a call as the callee and arguments the
   substitution settles, an end as `ParametricDrop` - so the pass has
   the whole body to clone. The pass resolves what those records left:
   a generic callee against the function its arguments instantiate,
   memoized by (item, arguments), and a dispatch against the
   implementation the receiver's type has (ADR-0080), while a builtin
   that satisfies an operator or `Display` without an implementation
   answers with the opcode or the inline expansion the concrete body
   would have used (ADR-0053). It rewrites each resolved call and
   substitutes the clone's types, names, and ends as it copies, and it
   runs before the borrow pass, so the borrow pass sees the clones and
   no parametric body. What remained then was the summary that carries
   the body.
4. Landed for the bodies that can cross. The summary carries a generic
   body, and a row that names one carries no `needs_body` mark. A
   generic item's row names the definition-site body it lowered to, and
   the dependent spells the row's declaration: a `Param` node names its
   parameter by index, a generic nominal's declaration is spelled from
   the instance the parametric bodies were built with, an impl declares
   its owner's parameters and each method only its own, and a path
   opens its package by the identity a `use` writes, so a signature
   resolves in the module that holds the item and in a dependent that
   reads the declaration. A carrying package copies into the
   dependent's tables what the carried bodies reach - their blocks,
   instructions, operands, the types they name, and the functions they
   call - with every reference rewritten through the map its entry
   moved through: every index keeps its meaning, and every name moves
   into the dependent's interner. Every module one artifact answers for
   shares the one merge, so a package's storage is copied once however
   many of its modules name it, and the specialization pass clones a
   carried body beside the dependent's own code. A function the summary
   names by symbol keeps no body: a reference to it is a call by that
   symbol, so the library's object stays the code that runs, and a
   library object now internalizes everything its surface does not
   name, which keeps a promoted symbol of a split build from meeting
   another object's copy, and a request that names no surface at all -
   a binary's relocatable object, a module emission - keeps every
   symbol, because nothing says which ones are a surface. A build
   therefore holds what its carried bodies reach rather than the
   library's whole lowered storage, and its reported count excludes
   what the library lowered and the dependent does not reach. An item
   the once-check cannot stand in for (item 5) has no body the summary
   can spell: its row keeps the `needs_body` mark and names only a
   declaration, and a dependent that calls one is told the item is
   unavailable rather than reading a body the library could not carry.
5. Landed for the items the once-check stands in for. The pass runs
   before the concrete body checks, so a call site knows the body was
   checked: it registers the item's signature and marks the entry - the
   instantiation's slot, or `PARAM_INST` for a spec method - for the
   readers that key on it, and runs no body check. A dispatch the body
   recorded resolves against
   the receiver's own implementation - the concrete row, a
   definition-site row whose nominal is the receiver's, or the
   checked-impl table's declaration, whose method item the pass clones
   under the receiver's arguments - and the pass answers per item
   whether it stood in, so only the items it did skip the walk. What
   cannot stand in keeps its per-instantiation check: a value
   parameter, a `comp fn`, an owning `fn()` slot or a parameter bounded
   by the `Fn` spec (ADR-0116 carries the closure's half), an
   intrinsic's shape a bare parameter would decide, and a body whose
   compile-time value reads a parameter's layout.
6. Landed, with a boundary named here. The trait answers a dependent's
   passes read travel in the same artifact (ADR-0085), so a carried
   body's ends and moves decide as the library decided them. A
   destructor's body does not cross with its type yet: a carried body
   that ends a value whose destructor the library recorded is refused -
   naming the type and the call that instantiated the body - rather
   than skipped, which would leak. Because a dependency a summary
   answers for is not read from source (ADR-0078), the refusal fires
   once a fresh summary answers for the library and not while its
   sources do: a program built against the sources is accepted where
   the same program built against the summary is refused. The summary
   keeps its priority because it is the interface; closing the gap is
   the destructor's body crossing, which this record does not settle.
