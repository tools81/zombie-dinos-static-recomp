import struct,sys,zlib,glob
fs=sys.argv[2:];cols=6;S=3
def rd(f):
    b=open(f,'rb').read();o=struct.unpack('<I',b[10:14])[0];w,h=struct.unpack('<ii',b[18:26]);h=abs(h)
    return w,h,b[o:]
im=[rd(f) for f in fs];w,h,_=im[0];tw=w//S;vs=1 if h<=300 else 2;th=h//vs
vs2=max(1,round(th/(tw*0.75)))
th=th//vs2 if vs2>1 else th
rows=(len(im)+cols-1)//cols;W=cols*tw;H=rows*th;buf=[bytearray(W*3) for _ in range(H)]
for n,(ww,hh,px) in enumerate(im):
    for y in range(th):
        sy=y*hh//th
        for x in range(tw):
            p=(sy*ww+x*ww//tw)*4
            X=((n%cols)*tw+x)*3;buf[n//cols*th+y][X:X+3]=bytes((px[p+2],px[p+1],px[p]))
def ch(t,d):return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+ch(b'IDAT',zlib.compress(b''.join(b'\0'+bytes(r) for r in buf)))+ch(b'IEND',b''))
