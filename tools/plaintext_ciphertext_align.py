#!/usr/bin/env python3
"""
plaintext_ciphertext_align.py - cross-checks a coclassic DLL's plaintext_<pid>.log (outbound
SendMsg plaintext, src/plaintext_log.cpp) against the launcher's relay_packets.log ciphertext
for the same session, in order:

  For each OUTBOUND plaintext entry, in sequence, matched against the next unconsumed
  "client->target" chunk in relay_packets.log for the given connection id.

Reports, per pair: whether the sizes match (confirms "no framing added downstream of
SendMsg" -- see plaintext_log.h) or diverge (framing/merging is happening; the offset is
reported so a keystream attempt can account for it rather than silently misalign). For every
size-matched pair, also computes and prints the XOR keystream (plaintext ^ ciphertext) for
that stretch -- a byproduct of the alignment check, not a decrypt attempt in itself, since
the cipher is a confirmed static-start positional stream (CONNECTION_DECIPHER_PREP.md).

Usage:
    python tools/plaintext_ciphertext_align.py --plaintext build/bin/Release/plaintext_12345.log \
        --relay build/bin/Release/relay_packets.log --conn 1
"""
import argparse, pathlib, re, sys

HEX_LINE_RE = re.compile(r'^\s+([0-9a-f]{6})\s+((?:[0-9a-f]{2}\s+){1,16})')
OUT_HEADER_RE = re.compile(r'\[OUT #(\d+)\] wireSize=(\d+)')
RELAY_HEADER_RE = re.compile(r'\[conn (\d+)\] (client->target) (\d+) bytes')


def parse_plaintext_log(path: pathlib.Path):
    """Yields (seq, size, bytes) for each OUT entry, in file order."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    i = 0
    while i < len(lines):
        m = OUT_HEADER_RE.search(lines[i])
        if not m:
            i += 1
            continue
        seq, size = int(m.group(1)), int(m.group(2))
        data = bytearray()
        i += 1
        while i < len(lines) and HEX_LINE_RE.match(lines[i]):
            hexpart = HEX_LINE_RE.match(lines[i]).group(2)
            data.extend(int(b, 16) for b in hexpart.split())
            i += 1
        yield seq, size, bytes(data[:size])


def parse_relay_log(path: pathlib.Path, conn_id: int):
    """Yields (size, bytes) for each client->target chunk on the given connection, in order."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    i = 0
    while i < len(lines):
        m = RELAY_HEADER_RE.search(lines[i])
        if not m or int(m.group(1)) != conn_id:
            i += 1
            continue
        size = int(m.group(3))
        data = bytearray()
        i += 1
        while i < len(lines) and HEX_LINE_RE.match(lines[i]):
            hexpart = HEX_LINE_RE.match(lines[i]).group(2)
            data.extend(int(b, 16) for b in hexpart.split())
            i += 1
        yield size, bytes(data[:size])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plaintext", type=pathlib.Path, required=True)
    ap.add_argument("--relay", type=pathlib.Path, required=True)
    ap.add_argument("--conn", type=int, required=True, help="connection id in relay_packets.log to align against")
    ap.add_argument("--show-keystream", action="store_true", help="print the hex keystream for each matched pair")
    a = ap.parse_args()

    plain = list(parse_plaintext_log(a.plaintext))
    cipher = list(parse_relay_log(a.relay, a.conn))
    print(f"{len(plain)} outbound plaintext entries, {len(cipher)} relay ciphertext chunks for conn {a.conn}")

    matched, mismatched = 0, 0
    ci = 0
    for seq, size, pbytes in plain:
        if ci >= len(cipher):
            print(f"OUT #{seq}: no more ciphertext chunks to align against (plaintext log has more entries "
                  f"than the relay captured -- session may have ended mid-log, or sizes already desynced)")
            break
        csize, cbytes = cipher[ci]
        ci += 1
        if size == csize:
            matched += 1
            keystream = bytes(p ^ c for p, c in zip(pbytes, cbytes))
            tag = "MATCH"
            extra = f" keystream={keystream.hex()}" if a.show_keystream else ""
        else:
            mismatched += 1
            tag = "SIZE MISMATCH"
            extra = f" (plaintext={size}, ciphertext={csize}, diff={csize - size:+d})"
        print(f"OUT #{seq}: wireSize={size} <-> relay chunk #{ci} size={csize}  [{tag}]{extra}")

    print(f"\n{matched} matched, {mismatched} mismatched out of {matched + mismatched} aligned pairs.")
    if mismatched:
        print("Mismatches mean framing/merging happens between SendMsg and the socket for at least some "
              "packets -- do NOT trust a naive 1:1 keystream from those; the offset needs characterizing "
              "before XORing across a mismatch.")
    elif matched:
        print("Clean 1:1 size match throughout -- confirms no framing is added downstream of SendMsg for "
              "this session, and every keystream byte above is valid.")


if __name__ == "__main__":
    main()
