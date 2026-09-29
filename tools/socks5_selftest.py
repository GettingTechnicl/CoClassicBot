#!/usr/bin/env python3
"""
socks5_selftest.py - raw SOCKS5 CONNECT handshake against the local pass-through, to prove
end-to-end that it can actually reach a real target (default: the login server), not just
that its listen port is open. Same handshake shape the launcher's Socks5Relay uses
(greeting 05 01 00, one-shot CONNECT with a domain-name ATYP) -- recreates the check
tunnel_probe.py did ad hoc, as a permanent repo tool. Connects, verifies the SOCKS5 success
reply, then closes -- does not send any game traffic.

Usage:
    python tools/socks5_selftest.py                       # -> 127.0.0.1:1080 -> login server
    python tools/socks5_selftest.py --proxy-port 1080 --host login.conqueronline.net --port 9959
"""
import argparse, socket, struct, sys, time


def socks5_connect(proxy_host, proxy_port, target_host, target_port, timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    t0 = time.perf_counter()
    s.connect((proxy_host, proxy_port))

    s.sendall(b"\x05\x01\x00")  # greeting: SOCKS5, 1 method, no-auth
    greet_reply = s.recv(2)
    if greet_reply != b"\x05\x00":
        return False, f"greeting rejected: {greet_reply!r}", None

    host_bytes = target_host.encode("ascii")
    req = b"\x05\x01\x00\x03" + bytes([len(host_bytes)]) + host_bytes + struct.pack(">H", target_port)
    s.sendall(req)
    reply = s.recv(10)
    elapsed = time.perf_counter() - t0
    s.close()

    if len(reply) < 2:
        return False, f"short reply: {reply!r}", elapsed
    if reply[1] != 0x00:
        codes = {1: "general failure", 2: "not allowed", 3: "network unreachable",
                 4: "host unreachable", 5: "connection refused", 6: "TTL expired",
                 7: "command not supported", 8: "address type not supported"}
        return False, f"SOCKS5 error 0x{reply[1]:02X} ({codes.get(reply[1], 'unknown')})", elapsed
    return True, "CONNECT succeeded", elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy-host", default="127.0.0.1")
    ap.add_argument("--proxy-port", type=int, default=1080)
    ap.add_argument("--host", default="login.conqueronline.net")
    ap.add_argument("--port", type=int, default=9959)
    ap.add_argument("--timeout", type=float, default=10.0)
    a = ap.parse_args()

    print(f"socks5_selftest: {a.proxy_host}:{a.proxy_port} -> CONNECT {a.host}:{a.port}")
    try:
        ok, msg, elapsed = socks5_connect(a.proxy_host, a.proxy_port, a.host, a.port, a.timeout)
    except OSError as e:
        print(f"FAIL: could not reach the pass-through at {a.proxy_host}:{a.proxy_port} ({e})")
        sys.exit(1)

    if ok:
        print(f"PASS: {msg} in {elapsed * 1000:.1f}ms")
        sys.exit(0)
    else:
        print(f"FAIL: {msg}" + (f" ({elapsed * 1000:.1f}ms)" if elapsed else ""))
        sys.exit(1)


if __name__ == "__main__":
    main()
