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

## Running log — PC-side notes for the VM-side instance

Standing practice (per the user, 2026-09-18): the PC-side instance posts anything relevant here
as it happens, so the user doesn't have to manually relay it. Newest entries on top. Each entry
should be skimmable — point to the real detail (a commit, a doc) rather than duplicating it.

### 2026-09-18 — proxy-mode kill-switch + relay logging bugs, both fixed

If you ever see (or have seen) a proxied account launch report "failed" in the account manager
despite the game actually logging in fine, or `relay_packets.log` being empty or only showing one
direction of traffic — that's a real bug, already found and fixed, not user error. Two issues in
`injector/main.cpp`'s `Socks5Relay`/`RelayLogger` (commit `605e4b9`):

1. `RelayLogger` was fully implemented but never actually instantiated in the live proxy-mode UI
   path — `relay.Start()` always got a null logger, so packet logging silently did nothing.
2. The kill-switch fires on *any* established tunnel closing, with no way to distinguish a real
   proxy failure from the account server's own normal disconnect-after-handoff (client auths
   against account server, gets the game server's address, disconnects, reconnects elsewhere —
   standard flow, not a failure). Since the relay currently only ever proxies the account/login
   leg (the game server's address is dynamic, delivered via `MsgConnectEx`, never in
   `servers.json` — so that leg bypasses the relay entirely), this **kill-switch was misfiring on
   every successful login**, not just failures.

**Kill-switch is currently disarmed** (`m_killSwitch = false`) as a temporary fix — see the
`⚠️ TODO` in `docs/investigation/CONNECTION_DECIPHER_PREP.md` for what re-arming it properly
needs. If you're debugging something proxy-related, know that fail-closed protection is
currently off, not that the relay is newly broken.

Also: if you ever find `servers.json` in the game install dir showing `"address": "127.0.0.1"`
for the login servers instead of the real `login.conqueronline.net`, that's `ServerConfigPatch`
having patched it for a proxy-mode run and never restoring it (process killed before clean
shutdown, e.g. by the kill-switch bug above) — not a config someone intentionally set. Safe to
fix by hand (restore `login.conqueronline.net`) if the game won't connect and you see this.

### 2026-09-18 — new debug/diagnostic tooling in the overlay and build

- Overlay's Packets tab and the launcher's relay logging now label packets by name (e.g.
  `Type=0x3F2 (MsgAction)`) using `src/msg_types.h`, a message-ID table sourced from the CO
  Development Wiki. Useful if you're ever staring at a raw packet capture.
- Map tab's ground-items table has a **"→ Pickup Test" button** per row — auto-fills the
  Developer Tools "Debug: Native Pickup Test" panel's Item ID/X/Y instead of needing them
  hand-transcribed from a screenshot.
- That debug panel also gained a **"Skip jump"** checkbox — sends the pickup packet alone,
  useful if you ever need an isolated, deterministic packet send for any kind of wire-level
  debugging (it logs the exact plaintext bytes it built, too).
- Three new standalone diagnostic DLLs exist now (`netfinder`, `recvfinder`, `flushfinder` —
  all read-only Winsock-hook tools, safe to inject into an already-running game process without
  a relaunch): capture outbound/inbound wire bytes for debugging network-layer issues. See
  `docs/investigation/CONNECTION_DECIPHER_PREP.md` for what they were built for and how to use
  them if a live network-layer mystery ever comes up on your side (e.g. the disconnect
  investigation) — `flushfinder` specifically is superseded/not recommended, see that doc's own
  note before reaching for it.

None of the above should affect a normal unattended bot run — they're opt-in debug tooling, not
changes to hunt/travel/combat logic.
