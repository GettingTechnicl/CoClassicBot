#!/usr/bin/env python3
"""
latency_baseline.py - Phase 1 Step 0: measure real round-trip time to the login and game
servers BEFORE any traffic is routed through the relay, so "how much latency did the relay
add" has a real number to compare against. Pure network test: opens a TCP connection and
closes it immediately. Does not touch the game, the relay, or any files under the game folder.

Measures TCP handshake RTT (time from issuing connect() to the socket becoming writable,
i.e. SYN -> SYN-ACK -> ACK completing) — the same thing tunnel_probe.py's approach relies on,
just turned into a repeatable, reported baseline instead of a one-off check.

Usage (no admin needed):
    python tools/latency_baseline.py
    python tools/latency_baseline.py --samples 50 --gap-ms 200
    python tools/latency_baseline.py --host 1.2.3.4 --port 5816 --label custom
"""
import argparse, socket, statistics, sys, time

DEFAULT_TARGETS = [
    ("login-server", "148.113.160.82", 9959),
    ("game-server", "148.113.198.18", 5816),
]


def one_sample(host, port, timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    t0 = time.perf_counter()
    try:
        s.connect((host, port))
        rtt = time.perf_counter() - t0
        return rtt, None
    except OSError as e:
        return None, str(e)
    finally:
        try:
            s.close()
        except OSError:
            pass


def percentile(values, p):
    if not values:
        return None
    s = sorted(values)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    return s[f] if f == c else s[f] + (s[c] - s[f]) * (k - f)


def run_target(label, host, port, samples, gap_ms, timeout):
    print(f"\n=== {label}: {host}:{port} ({samples} samples) ===")
    ok, errs = [], []
    for i in range(samples):
        rtt, err = one_sample(host, port, timeout)
        if rtt is not None:
            ok.append(rtt * 1000.0)
        else:
            errs.append(err)
        if i < samples - 1:
            time.sleep(gap_ms / 1000.0)
    if not ok:
        print(f"  ALL {samples} attempts failed. Last error: {errs[-1] if errs else 'n/a'}")
        return None
    ok.sort()
    result = dict(
        label=label, host=host, port=port, n=len(ok), failed=len(errs),
        median=statistics.median(ok), p95=percentile(ok, 0.95), p99=percentile(ok, 0.99),
        min=ok[0], max=ok[-1],
    )
    print(f"  ok={result['n']} failed={result['failed']}  "
          f"min={result['min']:.2f}ms  median={result['median']:.2f}ms  "
          f"p95={result['p95']:.2f}ms  p99={result['p99']:.2f}ms  max={result['max']:.2f}ms")
    if errs:
        uniq = sorted(set(errs))
        print(f"  errors seen: {', '.join(uniq[:5])}")
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=30)
    ap.add_argument("--gap-ms", type=int, default=300, help="delay between samples, avoids hammering the server")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--host", help="add one custom target instead of / in addition to the defaults")
    ap.add_argument("--port", type=int)
    ap.add_argument("--label", default="custom")
    ap.add_argument("--only-custom", action="store_true", help="skip the built-in login/game targets")
    a = ap.parse_args()

    targets = [] if a.only_custom else list(DEFAULT_TARGETS)
    if a.host and a.port:
        targets.append((a.label, a.host, a.port))
    elif a.host or a.port:
        print("--host and --port must be given together"); sys.exit(2)

    print(f"latency_baseline: {len(targets)} target(s), {a.samples} samples each, "
          f"{a.gap_ms}ms gap, {a.timeout}s connect timeout")
    print("This is a raw TCP-handshake baseline (no relay involved) — compare Phase 1's "
          "relay-added overhead against these numbers once the gateway exists.")

    results = [run_target(label, host, port, a.samples, a.gap_ms, a.timeout) for label, host, port in targets]
    results = [r for r in results if r]
    if results:
        print("\n=== summary ===")
        for r in results:
            print(f"  {r['label']:14s} median={r['median']:6.2f}ms  p95={r['p95']:6.2f}ms  p99={r['p99']:6.2f}ms")
        print("\nProposed Phase 1 gate: relay-added RTT median <= 1ms, p99 <= 3ms, measured "
              "against a loopback echo target once the gateway is built (that comparison needs "
              "the gateway to exist, so it happens at Step 2, not here).")


if __name__ == "__main__":
    main()
