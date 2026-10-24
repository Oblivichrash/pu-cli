# Releases

How a version is cut, and what belongs in `CHANGELOG.md`. The history itself is in
[CHANGELOG.md](../CHANGELOG.md); this document owns the rules that produce it.

## Where the version lives

`project(pu-cli VERSION x.y.z)` in [CMakeLists.txt](../CMakeLists.txt) is the only copy. No
source file, README or document restates it, so there is no second place to forget.

## When a version is cut

Work accumulates under `## Unreleased`. A release is a commit that:

1. moves `## Unreleased` to `## x.y.z` and starts a fresh, empty `## Unreleased` above it,
2. sets `VERSION` in `CMakeLists.txt` to match,
3. adds a row to the **Upgrading** table in [README.md](../README.md) when a reader must act,
4. tags the commit `vx.y.z`.

The order of the first two does not matter; the tag does, because it is what a user checks
out. A version is never reused and a released section is never edited afterwards — a mistake
in one is corrected by the next release, not by rewriting what a user already read.

## What an entry is

**One entry is one change a user can see.** Not one commit, and not one file. A refactor that
changes no behaviour has no entry; a bug fix and the test that pins it are one entry.

An entry says what was true before and what is true now, in that order, because a reader is
looking for the thing that changed under them:

> A cancelled turn stores every tool call it never ran as `interrupted` rather than leaving it
> `pending`, so it no longer blocks `/rewind` or an agent switch.

The heading a change goes under is decided by what a reader must do, not by what the code did:

| Heading | Holds |
| --- | --- |
| `Added` | A capability that did not exist |
| `Changed` | Something that behaves differently and needs no action |
| `Fixed` | Something that was wrong and is now right |
| `Removed` | A capability taken away, and what replaces it |
| `Breaking` | A change a reader must act on. It also gets a row in the README Upgrading table and a leading `**Breaking**` in the entry |

Each heading appears at most once per section. If a change fits two, it goes where the reader
must act first.

## What is not an entry

- A dependency bump with no behaviour behind it.
- A comment, a rename internal to a module, or a test added on its own.
- A document correction that does not change what a reader would do.

These belong to the commit that made them. The CHANGELOG is a record of what a user gets, not
a record that the repository exists.

## Length

There is no line limit, and Unreleased is expected to be the longest section because it holds
every change since the last release. The limit is on the *entry*: two or three sentences, one
fact, no restatement of the heading. An entry that needs a table or a diagram is a document
that CHANGELOG should link to.
