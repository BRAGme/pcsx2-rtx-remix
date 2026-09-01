# Contributing

This is a research fork of PCSX2 that submits PS2 geometry to the RTX Remix runtime. It is not
finished, and the most valuable help is not code.

If you only read one section, read [The one rule](#the-one-rule).

---

## What helps most

**A per-game `.conf` you tuned.** This is the top of the list by some distance. It touches no code,
cannot break another title, and `tools/package-release.sh` picks up `bin/*.conf` automatically, so
a good config ships in the next build. Six exist today and there are hundreds of PS2 games.

**A log from a title nobody has tried.** `logs\emulog.txt` from a game not in the table tells us
whether the camera solved, how many draws were submitted, and whether anything was rejected —
often enough to say what a title needs without anyone else owning the disc.

**A reproduction for something already broken.** The known blockers are in the
[README](README.md#known-blockers). A specific title where one of them reproduces on demand is
worth more than a general report, because it turns a vague fault into something measurable.

**Code**, if you want to. See [Building](README.md#building) first — a fresh clone does not build
until you supply `deps/`, and the failure looks like a broken repo rather than a missing step.

---

## The one rule

**Only write down numbers you actually measured on the title you are describing.**

This matters more here than in most projects, because configs and comments in this repo are written
as evidence. Someone tuning the next game reads `MINRT = 65536` with a comment explaining the render
target was 128×128, and trusts it. If that number was a guess dressed as a measurement, they tune
against fiction and have no way to find out.

An unverified value is fine. An unverified value **labelled as unverified** is genuinely useful —
it says "this is where to start" without claiming more. What is not fine is the two being
indistinguishable.

[`bin/SCUS-97275.conf`](bin/SCUS-97275.conf) is the model. Its header says plainly:

> `NOTHING BELOW HAS BEEN MEASURED ON THIS TITLE. Every number here was measured on Combined`
> `Assault. Treat this as a starting hypothesis and confirm against this game's own counters`
> `before quoting any of it as fact.`

That file is more useful than one full of confident numbers, because you can tell exactly how much
to trust it.

This is not hypothetical. The identical line `MEASURED on this title: empty windows 1319 held 0`
once appeared in three different games' configs — one measurement, copy-pasted twice — and a God of
War profile shipped headed "SOCOM II". Both were caught by reading, not by any tool.

---

## AI-assisted work

**Welcome, no disclosure required.** Much of this fork was written with AI assistance and the
commits say so in a `Co-Authored-By` trailer. It will not count against your contribution.

The caveat is the rule above, and it is worth stating because it is the specific way this goes
wrong: **a model asked to fill in a config will invent measurements that look exactly like real
ones.** Confident, plausible, correctly formatted, and untrue. The two examples in the previous
section were both produced that way.

So use whatever tools you like, then check the numbers against the actual game before you write them
down as fact — or label them as unchecked. That is the whole ask.

---

## How to send something

**No git needed.** Open an issue — there are templates for
[a config](.github/ISSUE_TEMPLATE/game_config.yaml) and
[a bug](.github/ISSUE_TEMPLATE/remix_bug_report.yaml) — and paste the file. It gets committed with
you credited as co-author.

**With git:** fork, branch, commit, open a pull request against `remix-backend`. That is the default
branch, so a fork lands on it automatically. Merging keeps your name as the author of your commits.

Either way you are credited, and anything merged ships in the next release.

---

## Style, if you are writing prose or comments

Not rules, just what the existing files do:

- Measured numbers rather than adjectives. "2,468,536 lit pixels against 1,110,107" beats "much
  brighter".
- Say what was refuted as well as what worked. Several comments in this repo exist only to stop the
  next person re-running a test that already failed, which is worth as much as the tests that
  passed.
- Record why a value is what it is, not just what it is. A bare list of knobs helps nobody tune the
  next title.

---

## What belongs upstream

If it reproduces with the Remix renderer **off**, it is an upstream PCSX2 issue and they can help
far more than we can. Test that first — it takes one setting change and saves everyone time.
