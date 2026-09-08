import json, os
HERE=os.path.dirname(os.path.abspath(__file__))
maps={m['id']:m for m in json.load(open(r'C:\Users\TerryGluff\Documents\Claude\CO99\CoClassicBot\data_maps.json'))}
d=json.load(open(os.path.join(HERE,'v8.json')))
print('pos',d['hero_stats']['pos'],'level',d['hero_stats']['level'])
print('=== map_deref ===')
for m in d.get('map_deref',[]):
    u=m['u32']
    cand=[(hex(i*4),v,maps[v]['name']) for i,v in enumerate(u) if 1000<=v<=1600 and v in maps]
    print(' ptr_rva',m['ptr_rva'],'obj',m['obj'],'vtable_rva',m['vtable_rva'])
    print('   docId candidates in object head:', cand[:8])
print('=== equipment (typeId + full u32) ===')
items={118355:'GoldCoronet',120095:'GoldNecklace',130455:'DemonArmor',480068:'Lathee'}
for e in d.get('equipment',[]):
    print(' slot',e['slot'],'typeId',e['typeId'],items.get(e['typeId'],'?'))
    u=e['u32']
    print('   u32:', u)
