import struct,sys
d=open(sys.argv[1],'rb').read()
assert d[:4]==b'XEX2'
n=struct.unpack('>I',d[0x14:0x18])[0]
for i in range(n):
    k,v=struct.unpack('>II',d[0x18+i*8:0x20+i*8])
    if k==0x00040006:
        mid,ver,bver,tid=struct.unpack('>IIII',d[v:v+16])
        print('media_id %08X title_id %08X version %08X base %08X'%(mid,tid,ver,bver))
    if k==0x000103FF:
        size,strsz,cnt=struct.unpack('>III',d[v:v+12])
        names=d[v+12:v+12+strsz].split(b'\0'); print('libs',[x.decode() for x in names if x])
