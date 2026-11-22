import json,sys,struct,re
data=open(sys.argv[1],'rb').read()
u32=lambda o: struct.unpack_from('<I',data,o)[0]
for mt in re.finditer(rb'(.{4})([\x20-\x7e]{2,200})\x09\x00\x00\x00\x02\x00\x00\x00\x09\x00\x00\x00',data,re.S):
  if struct.unpack('<I',mt.group(1))[0]!=len(mt.group(2)): continue
  o=mt.end(); found=None
  for q in range(o,o+400):
    if u32(q+8) in (0x20,0x28,0x4c) and u32(q+12)==9 and u32(q+16)==2 and u32(q+4)==0 and 0<u32(q)<400: found=q;break
  print(hex(mt.start()), mt.group(2).decode(), 'found' if found else None, u32(found) if found else '', (found-o) if found else '', data[o:o+48].hex(' ') if not found else '')
