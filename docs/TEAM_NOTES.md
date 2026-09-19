# Team notes — two Claude instances on this project

Written 2026-09-18. If the user points you here, read this first — it explains who else is
working on this repo and how to stay coordinated.

## The arrangement

As of 2026-09-18 there are **two Claude Code instances** working on CoClassicBot, on two
different machines, with no shared memory or messaging between them:

- **The dedicated wired QEMU Windows 11 VM** — runs the bot(s) unattended for real, long
  sessions, against real accounts. That instance has direct access to the live running
  processes and to every log the bot produces as it runs
  (`build/bin/Release/coclassic_<pid>.log`, `launcher.log`). Its role is frontline debugging:
  watch what actually happens in unattended play, root-cause from live logs, fix what surfaces.
- **The user's regular PC** (wherever you're reading this from, if you're not the VM) — broader
  development: new features, refactors, anything not gated on watching a live unattended run.

Neither instance has any built-in visibility into the other's session, chat history, or local
memory files. **This repo is the only shared state.** If you land here from the PC side and the
VM-side instance found or fixed something, it will be in a commit, a commit message, or a doc
under `docs/` — not anywhere else. The reverse is also true: if you're doing dev work here that
the VM-side instance should know about before its next unattended run (a build/config change, a
vendored-dep bump, a behavior change that affects long-running sessions), leave it somewhere a
commit will carry it, since the user has to manually tell each instance to look.

## Where to look for VM-side context

- `docs/investigation/00_HANDOFF_STATE.md` — the disconnect-investigation handoff/status doc
  written for the original VM migration. Still the best entry point for that investigation's
  current state and the setup/build gotchas specific to running on that VM.
- `docs/investigation/` generally — the VM-side instance's working docs for whatever it's
  currently chasing (disconnect causes, live-observed bugs, RE notes). Check commit history /
  dates here for what's current vs. historical.
- Git log / commit messages on `master` — the actual source of truth for what shipped. Don't
  assume a doc is up to date with the latest commits; cross-check.

## If you're the VM-side instance reading this

Same applies in reverse — dev work landing from the PC side (new features, architecture changes)
will show up as commits you haven't seen reasoning about. Read recent commit messages after a
`git pull`/`git fetch` before assuming your own picture of the codebase is current, the same way
you'd want the PC side to do for your work.
