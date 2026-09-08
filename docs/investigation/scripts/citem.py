import json, os, struct
HERE=os.path.dirname(os.path.abspath(__file__))
d=json.load(open(os.path.join(HERE,'v8.json')))
eq={e['slot']:e for e in d['equipment']}

def tobytes(u32list):
    b=b''.join(struct.pack('<I',v) for v in u32list)
    return b

def find(b, val):
    hits=[]
    # u8
    for o in range(len(b)):
        if b[o]==val and val<256: hits.append((o,'u8'))
    # u16
    for o in range(0,len(b)-1):
        if struct.unpack_from('<H',b,o)[0]==val: hits.append((o,'u16'))
    # u32
    for o in range(0,len(b)-3):
        if struct.unpack_from('<I',b,o)[0]==val: hits.append((o,'u32'))
    return hits

def report(slot, name, known):
    b=tobytes(eq[slot]['u32'])
    print(f'\n=== slot {slot} {name} ===')
    for label,val in known.items():
        hits=find(b,val)
        # prefer aligned offsets, dedupe
        hs=', '.join(f'+0x{o:X}({t})' for o,t in hits[:6])
        print(f'  {label}={val}: {hs if hits else "(not found)"}')

report(0,'GoldCoronet', {'defense':50,'curDura':40,'maxDura':41})
report(3,'Lathee', {'strength':56,'enchHP':6,'curDura':15,'maxDura':27,'dex':10,'freq':100,'atkMin':46,'atkMax':140})
