import sys
S=sys.argv[1]
src=open(S+'/net2.py').read()
src=src.replace("def lab(m):\n    s=m['type_name']","""ENS=open('/Users/santiagovilanova/Downloads/AnimaMundi2_tips.ens','rb').read()
import re,struct
def ioname(m):
    o=int(m['offset'],16); b=ENS[o:o+0x400]; k=b.find(b'\\x00\\xef\\x22\\x00')
    seg=b[k+4+35*4:k+4+35*4+80]
    t=struct.unpack('<i',seg[:4])[0]
    n=struct.unpack('<i',seg[4:8])[0]
    if t&0xff00==0x500 and 0<n<40: return seg[8:8+n].decode('latin1')
    if t&0xff00==0x500 and n==0: return ''
    mm=re.search(rb'[ -~]{2,}',seg); return '?'+(mm.group().decode() if mm else '')
def lab(m):
    s=m['type_name']
    if s in ('In','Out'): s+=" <%s>"%ioname(m)""")
src=src.replace("def short(oid):\n    m=mods[oid]; return","def short(oid):\n    m=mods[oid]\n    if m['type_name'] in ('In','Out'): return '%s<%s>#%d'%(m['type_name'],ioname(m),oid)\n    return")
open(S+'/anima/pitch/net3.py','w').write(src)
