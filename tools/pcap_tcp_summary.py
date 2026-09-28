#!/usr/bin/env python3
"""
pcap_tcp_summary.py - read a pktmon .pcapng and report, per TCP flow on the game/login ports, exactly what a
disconnect needs: who sent the first FIN or RST, how long the flow had been silent before it, retransmits, and the
last packets. No third-party packages.

  python tools/pcap_tcp_summary.py snapshot.pcapng [--port 5816 --port 9959] [--tail 15]

Notes learned from the first real capture (2026-09-28):
  * pktmon logs the SAME physical packet at several capture points (the Wi-Fi layer shows it encrypted with a
    junk ethertype; the IP layer shows it decoded). Only decodable Ethernet/IPv4/TCP frames are used here.
  * The same TCP packet can also appear at more than one decodable point a few microseconds apart. Those are
    de-duplicated; a repeat of the same (direction, seq, len) more than 5 ms later is a genuine RETRANSMIT.
  * Timestamps are printed in local-independent UTC. The pcapng default resolution (microseconds) is assumed
    unless an if_tsresol option says otherwise.
"""
import argparse, collections, datetime, struct, sys


def read_blocks(data):
    i = 0
    tsresol = 1e-6
    while i + 12 <= len(data):
        bt, bl = struct.unpack_from('<II', data, i)
        if bl < 12 or i + bl > len(data):
            break
        body = data[i + 8:i + bl - 4]
        if bt == 1 and len(body) > 8:                       # interface description: look for if_tsresol (opt 9)
            o = 8
            while o + 4 <= len(body):
                code, ln = struct.unpack_from('<HH', body, o)
                if code == 0:
                    break
                if code == 9 and ln >= 1:
                    v = body[o + 4]
                    tsresol = 10.0 ** -(v & 0x7F) if not (v & 0x80) else 2.0 ** -(v & 0x7F)
                o += 4 + ((ln + 3) & ~3)
        elif bt == 6 and len(body) >= 20:                   # enhanced packet block
            iface, hi, lo, cap, orig = struct.unpack_from('<IIIII', body, 0)
            yield (hi << 32 | lo) * tsresol, body[20:20 + cap]
        i += bl


def tcp_of(frame):
    if len(frame) < 54 or struct.unpack('>H', frame[12:14])[0] != 0x0800:
        return None
    ihl = (frame[14] & 0xF) * 4
    if frame[23] != 6:
        return None
    t = 14 + ihl
    if len(frame) < t + 20:
        return None
    sp, dp, seq, ack, off, fl = struct.unpack('>HHIIBB', frame[t:t + 14])
    total = struct.unpack('>H', frame[16:18])[0]
    plen = max(0, total - ihl - ((off >> 4) * 4))
    src = '.'.join(map(str, frame[26:30])); dst = '.'.join(map(str, frame[30:34]))
    return src, sp, dst, dp, seq, ack, fl, plen


def flagstr(fl):
    return ''.join(n for b, n in ((0x02, 'S'), (0x01, 'F'), (0x04, 'R'), (0x08, 'P'), (0x10, 'A')) if fl & b) or '.'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcapng')
    ap.add_argument('--port', type=int, action='append')
    ap.add_argument('--tail', type=int, default=15)
    a = ap.parse_args()
    ports = set(a.port or [5816, 9959])
    data = open(a.pcapng, 'rb').read()

    flows = collections.OrderedDict()
    seen = {}
    dup = 0
    for ts, fr in read_blocks(data):
        r = tcp_of(fr)
        if not r:
            continue
        src, sp, dst, dp, seq, ack, fl, plen = r
        if sp not in ports and dp not in ports:
            continue
        server_side = sp in ports
        cli = (dst, dp) if server_side else (src, sp)
        srv = (src, sp) if server_side else (dst, dp)
        key = (cli, srv)
        f = flows.setdefault(key, dict(pk=[], retx=0))
        dkey = (key, server_side, seq, plen, fl)
        prev = seen.get(dkey)
        seen[dkey] = ts
        if prev is not None and ts - prev < 0.005:
            dup += 1
            continue
        retx = prev is not None and plen > 0 and not (fl & 0x02)
        if retx:
            f['retx'] += 1
        f['pk'].append((ts, 'S->C' if server_side else 'C->S', flagstr(fl), seq, ack, plen, retx))

    print('pcapng: %s  |  decodable game/login TCP flows: %d  |  same-packet duplicates dropped: %d' % (a.pcapng, len(flows), dup))
    utc = lambda t: datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%H:%M:%S.%f')[:-3] + 'Z'
    for (cli, srv), f in flows.items():
        pk = sorted(f['pk'])
        if not pk:
            continue
        print('\n=== %s:%d  <->  %s:%d ===' % (cli[0], cli[1], srv[0], srv[1]))
        print('  %d packets, %s -> %s, span %.1fs' % (len(pk), utc(pk[0][0]), utc(pk[-1][0]), pk[-1][0] - pk[0][0]))
        sb = sum(p[5] for p in pk if p[1] == 'S->C'); cb = sum(p[5] for p in pk if p[1] == 'C->S')
        print('  payload bytes: server->client %d, client->server %d;  retransmits: %d' % (sb, cb, f['retx']))
        gaps = [(pk[i + 1][0] - pk[i][0], i) for i in range(len(pk) - 1)]
        if gaps:
            g, gi = max(gaps)
            print('  longest silence between packets: %.2fs (at %s)' % (g, utc(pk[gi][0])))
        closers = [p for p in pk if 'F' in p[2] or 'R' in p[2]]
        if closers:
            c = closers[0]
            idx = pk.index(c)
            last_data = [p for p in pk[:idx] if p[5] > 0]
            since = (c[0] - last_data[-1][0]) if last_data else None
            who = 'the SERVER' if c[1] == 'S->C' else 'the CLIENT (this machine)'
            kind = 'RST (abort)' if 'R' in c[2] else 'FIN (orderly close)'
            print('  FIRST CLOSE: %s sent %s at %s  (%s after the last data packet)'
                  % (who, kind, utc(c[0]), ('%.2fs' % since) if since is not None else 'n/a'))
            others = [p for p in closers[1:]]
            for p in others[:4]:
                print('     then: %s %s at %s' % ('SERVER' if p[1] == 'S->C' else 'CLIENT', p[2], utc(p[0])))
        else:
            print('  no FIN/RST seen in this capture (connection still open at capture end)')
        print('  last %d packets:' % min(a.tail, len(pk)))
        for p in pk[-a.tail:]:
            print('    %s %s %-3s seq=%d ack=%d len=%d%s' % (utc(p[0]), p[1], p[2], p[3], p[4], p[5], '  RETRANSMIT' if p[6] else ''))


if __name__ == '__main__':
    main()
