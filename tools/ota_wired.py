import serial,time,struct,zlib,sys
PORT='/dev/ttyUSB0'; BIN='/tmp/firmware.bin'
def crc16(d):
    c=0xFFFF
    for x in d:
        c^=x
        for _ in range(8):c=(c>>1)^0xA001 if c&1 else c>>1
    return bytes([c&0xFF,(c>>8)&0xFF])
S=serial.Serial(PORT,115200,timeout=0.5)
def tx(frame): S.reset_input_buffer(); S.write(frame+crc16(frame))
def txn(frame,rl,to=1.0):
    S.reset_input_buffer(); S.write(frame+crc16(frame)); t=time.time(); buf=b''
    while len(buf)<rl and time.time()-t<to: buf+=S.read(rl-len(buf))
    return buf
def f03(r,n): return bytes([1,3,r>>8,r&0xFF,n>>8,n&0xFF])
def f05(r,on): return bytes([1,5,r>>8,r&0xFF,0xFF if on else 0,0])
def f06(r,v): return bytes([1,6,r>>8,r&0xFF,v>>8,v&0xFF])
def f10(r,regs):
    body=bytes([1,0x10,r>>8,r&0xFF,(len(regs)>>8)&0xFF,len(regs)&0xFF,len(regs)*2])
    for v in regs: body+=bytes([(v>>8)&0xFF,v&0xFF])
    return body
def rd(r,n=1):
    x=txn(f03(r,n),5+2*n)
    if len(x)>=5+2*n and x[1]==3: return [(x[3+2*i]<<8)|x[4+2*i] for i in range(n)]
    return None
# 0. 读app固件版本
fw=rd(0x3E02); print("升级前 app fw =", hex(fw[0]) if fw else None)
data=open(BIN,'rb').read()
if len(data)%2: data+=b'\xFF'
ln=len(data); cr=zlib.crc32(data)&0xFFFFFFFF
print("固件 %d字节 CRC32=%08X"%(ln,cr))
# 1. 触发进bootloader
print("触发OTA(线圈0x0020)..."); txn(f05(0x0020,1),8,to=0.5); time.sleep(2.0)
# 2. 确认在bootloader (0x7000=0xB007)
m=rd(0x7000); print("bootloader标志 0x7000 =", hex(m[0]) if m else None, "(应0xB007)")
if not m or m[0]!=0xB007: print("未进bootloader, 中止"); sys.exit(1)
# 3. 设长度+CRC32 (0x7010, 4寄存器)
regs=[ln>>16, ln&0xFFFF, cr>>16, cr&0xFFFF]; txn(f10(0x7010,regs),8); 
# 4. 擦除 (0x7020=1)
txn(f06(0x7020,1),8,to=3.0); 
st=rd(0x7001); print("擦除后 status 0x7001 =", st[0] if st else None, "(应1)")
# 5. 写数据块: LE半字, 每块32寄存器
hw=struct.unpack('<%dH'%(ln//2), data)
t0=time.time(); off=0; CH=32
for i in range(0,len(hw),CH):
    chunk=hw[i:i+CH]; r=txn(f10(0x7100,list(chunk)),8)
    if len(r)<8: print("块%d无应答"%i); 
print("写完 %d寄存器 用时%.1fs"%(len(hw),time.time()-t0))
prog=rd(0x7002,2); print("已写偏移 =", (prog[0]<<16|prog[1]) if prog else None,"(应=%d)"%ln)
# 6. 校验+跳转 (0x7020=2)
print("校验+跳转..."); txn(f06(0x7020,2),8,to=2.0); time.sleep(2.0)
# 7. 确认app回来
fw2=rd(0x3E02); print("升级后 app fw =", hex(fw2[0]) if fw2 else None, "← 回到app=OTA成功")
