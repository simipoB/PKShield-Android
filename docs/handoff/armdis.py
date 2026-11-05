import sys, capstone
img=open(sys.argv[1],'rb').read(); a=int(sys.argv[2],16); b=int(sys.argv[3],16)
md=capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for i in md.disasm(img[a:b], a): print(f'{i.address:08x}  {i.mnemonic:8s} {i.op_str}')
