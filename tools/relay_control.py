#!/usr/bin/env python3
"""
relay_control.py - operator CLI for the gateway's control protocol (injector/main.cpp's
ConnectGateway, Phase 2 of docs/investigation/CONTAINMENT.md). Talks to the SAME loopback
control port the DLL uses to request tunnels, so this is exactly what the game itself is
already trusted to reach -- nothing new exposed, no auth needed (loopback + this user only).

Commands:
    list                          active connections: id, destination, paused/delay state, age
    kill   <id> [--rst]           close a connection (default: FIN; --rst forces a hard reset)
    delay  <id> <ms>              add artificial latency to a connection (both directions, 0 = off)
    pause  <id>                   stop forwarding data on a connection (peer stalls for real)
    resume <id>                   undo a pause

Every deliberate action here is also written to relay_packets.log as "OPERATOR KILL/DELAY/
PAUSE/RESUME", clearly distinguishable from an organic event, so later forensic reading of that
log is never confused about which is which.

Port discovery: the control port is ephemeral and only known from the launcher's own log line
"Gateway control listening on 127.0.0.1:NNNNN" -- this reads the LAST such line from
relay_packets.log by default (same file the relay/gateway already write to). Pass --port to
skip discovery (e.g. if you already know it, or relay_packets.log has more than one account's
history and you want to be sure).

Usage:
    python tools/relay_control.py list
    python tools/relay_control.py kill 1000003 --rst
    python tools/relay_control.py delay 1000003 250
    python tools/relay_control.py pause 1000003
    python tools/relay_control.py resume 1000003
    python tools/relay_control.py --port 61391 list
"""
import argparse, pathlib, re, socket, sys

DEFAULT_LOG = pathlib.Path(__file__).resolve().parent.parent / "build" / "bin" / "Release" / "relay_packets.log"
PORT_RE = re.compile(r"Gateway control listening on 127\.0\.0\.1:(\d+)")


def discover_port(log_path: pathlib.Path) -> int:
    if not log_path.exists():
        raise SystemExit(f"no log at {log_path} -- pass --port, or --log to point at the right relay_packets.log")
    last = None
    # the file can be large and is append-only; a plain read is fine for a CLI tool run
    # occasionally by hand, and simplest to get right.
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PORT_RE.search(line)
            if m:
                last = int(m.group(1))
    if last is None:
        raise SystemExit(f"no 'Gateway control listening' line found in {log_path} -- is proxy mode currently on for this account?")
    return last


def send_command(port: int, line: str, timeout: float = 5.0) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        try:
            s.connect(("127.0.0.1", port))
        except OSError as e:
            raise SystemExit(f"could not reach the gateway control port 127.0.0.1:{port} ({e}) -- "
                              f"is the account still running with proxy mode on?")
        s.sendall((line + "\n").encode("ascii"))
        s.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks).decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, help="gateway control port (skips log-based discovery)")
    ap.add_argument("--log", type=pathlib.Path, default=DEFAULT_LOG, help="relay_packets.log to discover the port from")
    sub = ap.add_subparsers(dest="action", required=True)

    sub.add_parser("list", help="show active connections")

    p_kill = sub.add_parser("kill", help="close a connection")
    p_kill.add_argument("id", type=int)
    p_kill.add_argument("--rst", action="store_true", help="force a hard reset instead of a graceful FIN")

    p_delay = sub.add_parser("delay", help="set artificial latency on a connection (0 = off)")
    p_delay.add_argument("id", type=int)
    p_delay.add_argument("ms", type=int)

    p_pause = sub.add_parser("pause", help="stop forwarding data on a connection")
    p_pause.add_argument("id", type=int)

    p_resume = sub.add_parser("resume", help="undo a pause")
    p_resume.add_argument("id", type=int)

    a = ap.parse_args()
    port = a.port if a.port else discover_port(a.log)

    if a.action == "list":
        reply = send_command(port, "LIST")
    elif a.action == "kill":
        reply = send_command(port, f"KILL {a.id} {'RST' if a.rst else 'FIN'}")
    elif a.action == "delay":
        if a.ms < 0:
            raise SystemExit("ms must be >= 0")
        reply = send_command(port, f"DELAY {a.id} {a.ms}")
    elif a.action == "pause":
        reply = send_command(port, f"PAUSE {a.id}")
    elif a.action == "resume":
        reply = send_command(port, f"RESUME {a.id}")
    else:
        raise SystemExit(2)

    sys.stdout.write(reply if reply.endswith("\n") else reply + "\n")
    if reply.startswith("ERR"):
        sys.exit(1)


if __name__ == "__main__":
    main()
