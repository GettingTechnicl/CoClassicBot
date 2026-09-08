import json, os, struct
HERE=os.path.dirname(os.path.abspath(__file__))
d=json.load(open(os.path.join(HERE,'v8.json')))
eq={e['slot']:e for e in d['equipment']}
def u32(slot,off): return eq[slot]['u32'][off//4]
def search_range(slot, lo, hi):
    return [(f'+0x{i*4:X}', v) for i,v in enumerate(eq[slot]['u32']) if lo<=v<=hi]

print('confirmed fixed fields (from itemtype copy):')
for slot,name in [(0,'GoldCoronet'),(3,'Lathee')]:
    print(f'  {name}: price@0x48={u32(slot,0x48)}  defense@0x54={u32(slot,0x54)}  maxDura@0x64={u32(slot,0x64)}')
print()
print('GoldCoronet current-dura candidates (want ~4000-4198, displays 40):')
print('  ', search_range(0, 3900, 4198))
print('Lathee current-dura candidates (want ~1400-1600, displays 15):')
print('  ', search_range(3, 1400, 1700))
# also show the two u32 right before maxDura for both
for slot,name in [(0,'GoldCoronet'),(3,'Lathee')]:
    print(f'{name} around maxDura (0x5C..0x68):', [(hex(o),u32(slot,o)) for o in range(0x5C,0x6C,4)])
