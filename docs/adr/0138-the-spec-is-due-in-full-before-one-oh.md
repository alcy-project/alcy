# ADR-0138: The spec is due in full before 1.0

- Subject: the language
- Status: Accepted
- Date: 2026-11-05

## Context

`docs/spec/` labeled its sections MVP, Bootstrap, staged, or post-MVP,
and `overview.md` called that set the staging labels. The labels were a
schedule: MVP named the surface the first self-contained compiler
would carry, post-MVP named what was deliberately held back, and a
reader took "in MVP" as "you may rely on this" and "post-MVP" as "you
may not".

ADR-0099 removed the schedule those labels described. 1.0 is the
compatibility line, declared when its conditions hold and not on a
date, and before it a wrong decision is fixed rather than kept. Two
things follow. Nothing in the specification is promised before 1.0, so
"in MVP or not" no longer separates a promise from a non-promise. And
every form the specification describes is due before 1.0, so post-MVP
named no later stage at all: it named work that is not done yet.

The labels then cost more than they returned. "MVP" was read both as
the running compiler, which had reached hello-world, and as the whole
surface the specification describes, which had not, and both readings
were in use in the same tree. A milestone label taken as a promise, in
a window where nothing is promised, hides the one distinction that
decides a question: implemented, or listed in `deferred.md`.

## Decision

The MVP and Post-MVP labels are retired. `docs/spec/` states the
language as it is meant to be, and everything it states is work toward
1.0; what is not implemented yet is `docs/spec/deferred.md`, which is
a list and not a stage. `overview.md` defines no label set, a
chapter's title carries no marker, and a sentence about a form says it
exists, or that it does not exist yet.

Two markers remain, because each answers a question the specification
still has to answer:

- Bootstrap (see `comp.md`, `fmt.md`, `ffi.md`) says a chapter
  describes the path to self-hosting.
- staged (see `types.md`) says a section is covered in part.

Neither says when work lands, and neither is a promise tier: the
marker says what a document covers, not what a reader may rely on.

Post-MVP is not replaced by post-1.0. Work parked today is due before
1.0 unless it is explicitly moved past it, and moving a marker past
the line is a decision of its own (ADR-0099).

## Consequences

- `deferred.md` is the single list of what is missing, and the only
  thing that has to be read to find out what is not implemented.
- ADR-0099's first condition now covers the whole specification rather
  than its MVP sections, and its Context no longer names MVP among the
  markers.
- Historical records keep their wording, including the title of
  `0005-llvm-only-mvp.md`: an ADR written while the labels were in
  force states what was true then. This record is the one to read for
  what the vocabulary means now.
- The diagnostic that said a form "arrives post-MVP" says it is not
  implemented yet, which is what a reader of a diagnostic needs.
- The roadmap's first 1.0 condition asks for everything the spec
  states, so the box no longer depends on which sections someone
  counted as MVP.

## Alternatives considered

- **Keep MVP and add post-1.0 beside it.** Rejected: it keeps a
  milestone label inside a window whose only line is 1.0, and it
  invents a stage ADR-0099 does not have.
- **Rename MVP to 0.1 wherever it appears.** Rejected: a version names
  a release, not a claim about a specification, and a section labeled
  0.1 reads as already shipped in a document that describes the whole
  language.
- **Delete the labels and say nothing.** Rejected: Bootstrap and staged
  would then be undefined, and a reader would take them for the
  promise tier this record is about.
- **Keep the labels and document that they are not promises.**
  Rejected: the vocabulary is read as a promise by everyone who meets
  it, so the documentation would fight the words.
