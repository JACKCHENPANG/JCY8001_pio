#!/usr/bin/env python3
"""JCY8001 OTA 在线升级 (USB/Modbus). 用法: python3 ota_flash.py app.bin [/dev/ttyUSB0]
流程: 先让 App 进 Bootloader (FC05 线圈 0x0020=1), 本工具检测到 0x7000=0xB007 后传固件.
"""
import serial,struct,time,zlib,sys
BIN=sys.argv[1] if len(sys.argv)>1 else "app.bin"
PORT=sys.argv[2] if len(sys.argv)>2 else "/dev/ttyUSB0"
ser=serial.Serial(PORT,115200,timeout=0.8)
def crc16(d):
    c=0xFFFF
    for b in d:
        c^=b
        for _ in range(8): c=(c>>1)^0xA001 if c&1 else c>>1
    return c
def xf(fr,n=6):
    fr=fr+struct.pack("<H",crc16(fr))
    for _ in range(n):
        ser.reset_input_buffer(); ser.write(fr); time.sleep(0.05); r=ser.read(256)
        if len(r)>=5 and r[0]==1 and (r[1]&0x80)==0: return r
    return r
def rd(reg):
    r=xf(struct.pack(">BBHH",1,3,reg,1)); return struct.unpack(">H",r[3:5])[0] if r and len(r)>=5 else None
def w6(reg,v): return xf(struct.pack(">BBHH",1,6,reg,v))
def w16(reg,regs):
    return xf(struct.pack(">BBHHB",1,0x10,reg,len(regs),len(regs)*2)+b"".join(struct.pack(">H",x) for x in regs))
def enter_boot():
    if rd(0x7000)==0xB007: return True
    xf(struct.pack(">BBHH",1,5,0x0020,0xFF00)); time.sleep(2)   # App: 进 Bootloader
    return rd(0x7000)==0xB007

if not enter_boot(): sys.exit("无法进入 Bootloader (0x7000!=0xB007)")
data=open(BIN,"rb").read()
if len(data)%2: data+=b"\xff"
ln=len(data); cr=zlib.crc32(data)&0xFFFFFFFF
print("固件 %d 字节 crc32=0x%08X"%(ln,cr))
w16(0x7010,[ln>>16,ln&0xFFFF,cr>>16,cr&0xFFFF])     # 设长度+CRC32
w6(0x7020,1)                                          # 擦除
t0=time.time()
while time.time()-t0<10 and rd(0x7001)!=1: time.sleep(0.2)
print("已擦除")
nreg=ln//2; regs=[data[2*i]|(data[2*i+1]<<8) for i in range(nreg)]
for i in range(0,nreg,60):
    w16(0x7100,regs[i:i+60])
    if (i//60)%10==0: print("  传输 %d/%d"%(min((i+60)*2,ln),ln))
print("传完, 写入偏移=%d"%((rd(0x7002)<<16)|rd(0x7003)))
w6(0x7020,2); time.sleep(1.0)                         # 完成: CRC校验→跳App
ok = (rd(0x7000)!=0xB007)                             # 不再是Bootloader = 已跳App = 成功
print("升级成功 ✅, App 已运行" if ok else "失败: CRC不过, 仍在Bootloader (status=%s)"%rd(0x7001))
