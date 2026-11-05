# xref.py <img> <text end> <addr>...: code that forms each address with ADRP + ADD/LDR (the usual way AArch64 code reaches a string or global)
import sys, struct
img=open(sys.argv[1],'rb').read(); tend=int(sys.argv[2],16); want={int(a,16) for a in sys.argv[3:]}
words=struct.unpack_from('<%dI'%(tend//4), img, 0)
hits={}
for i,w in enumerate(words):
    if (w & 0x9f000000)==0x90000000:   # ADRP
        rd=w&31; immlo=(w>>29)&3; immhi=(w>>5)&0x7ffff; imm=((immhi<<2)|immlo); imm = imm-(1<<21) if imm&(1<<20) else imm
        page=((i*4)&~0xfff)+(imm<<12)
        for j in range(i+1,min(i+6,len(words))):
            v=words[j]
            if (v & 0xffc00000)==0x91000000 and ((v>>5)&31)==rd:   # ADD Xd, Xrd, #imm12
                t=page+((v>>10)&0xfff)
                if t in want: hits.setdefault(t,[]).append(i*4)
                break
for t in sorted(want): print(hex(t), [hex(x) for x in hits.get(t,[])])
