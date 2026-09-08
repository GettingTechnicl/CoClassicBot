import re
data=open("image_dump.bin","rb").read()
def astrings(minlen=4):
    for m in re.finditer(rb"[\x20-\x7e]{%d,}"%minlen, data):
        yield m.start(), m.group().decode("latin1")
S=list(astrings())
protos=sorted(set(s for _,s in S if s.endswith(".proto")))
print("=== .proto files ===", len(protos))
for p in protos[:60]: print("  ",p)
msgnames=sorted(set(s for _,s in S if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{2,34}", s)
    and re.search(r"Walk|Attack|Move|Jump|Action|Pickup|Loot|Skill|Magic|Login|Trade|Talk|Team|Ping|Sync|Interact|Target|Move",s)))
print("=== action-ish names ===", len(msgnames))
for m in msgnames[:100]: print("  ",m)
