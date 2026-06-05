import asyncio,time,struct,zlib,sys
from bleak import BleakScanner,BleakClient
CHR="0000ffe1-0000-1000-8000-00805f9b34fb"
BIN="/tmp/firmware.bin"
def crc16(d):
    c=0xFFFF
    for x in d:
        c^=x
        for _ in range(8):c=(c>>1)^0xA001 if c&1 else c>>1
    return bytes([c&0xFF,(c>>8)&0xFF])
def f03(r,n):p=bytes([1,3,r>>8,r&0xFF,n>>8,n&0xFF]);return p+crc16(p)
def f05(r,on):p=bytes([1,5,r>>8,r&0xFF,0xFF if on else 0,0]);return p+crc16(p)
def f06(r,v):p=bytes([1,6,r>>8,r&0xFF,v>>8,v&0xFF]);return p+crc16(p)
def f10(r,regs):
    b=bytes([1,0x10,r>>8,r&0xFF,(len(regs)>>8)&0xFF,len(regs)&0xFF,len(regs)*2])
    for v in regs:b+=bytes([(v>>8)&0xFF,v&0xFF])
    return b+crc16(b)
async def find():
    devs=await BleakScanner.discover(timeout=12.0,return_adv=True)
    for a,(d,adv) in devs.items():
        if "jdy" in (d.name or adv.local_name or "").lower():return d
    return None
class C:
    def __init__(s,cl):s.cl=cl;s.buf=bytearray()
    def cb(s,_,d):s.buf.extend(d)
    async def start(s):await s.cl.start_notify(CHR,s.cb);await asyncio.sleep(0.3)
    async def txn(s,fr,rl,to=2.0):
        for _ in range(4):
            s.buf.clear();await s.cl.write_gatt_char(CHR,fr,response=False);t=time.time()
            while len(s.buf)<rl and time.time()-t<to:await asyncio.sleep(0.02)
            if len(s.buf)>=rl:break
            await asyncio.sleep(0.1)
        return bytes(s.buf)
    async def rd(s,r,n=1):
        x=await s.txn(f03(r,n),5+2*n)
        if len(x)>=5+2*n and x[1]==3:return [(x[3+2*i]<<8)|x[4+2*i] for i in range(n)]
        return None
async def main():
    d=await find()
    if not d:print("no JDY");return
    cl=BleakClient(d,timeout=15.0);await cl.connect();c=C(cl);await c.start()
    mtu=cl.mtu_size; print("MTU=",mtu)
    fw=await c.rd(0x3E02); print("升级前 app fw=",hex(fw[0]) if fw else None)
    data=open(BIN,'rb').read()
    if len(data)%2:data+=b'\xFF'
    ln=len(data);cr=zlib.crc32(data)&0xFFFFFFFF
    print("固件 %d字节 CRC32=%08X"%(ln,cr))
    # 触发OTA
    print("触发OTA...");await c.txn(f05(0x0020,1),8,to=0.6)
    await cl.disconnect();await asyncio.sleep(3.0)
    # 重连(复位后)
    d=await find()
    if not d:print("复位后没扫到JDY");return
    cl=BleakClient(d,timeout=15.0);await cl.connect();c=C(cl);await c.start()
    m=await c.rd(0x7000); print("bootloader 0x7000=",hex(m[0]) if m else None,"(应0xB007)")
    if not m or m[0]!=0xB007:print("未进bootloader");return
    await c.txn(f10(0x7010,[ln>>16,ln&0xFFFF,cr>>16,cr&0xFFFF]),8)
    await c.txn(f06(0x7020,1),8,to=3.0)
    st=await c.rd(0x7001);print("擦除 status=",st[0] if st else None)
    hw=struct.unpack('<%dH'%(ln//2),data)
    # 块大小: 一帧≤一个BLE write(≤MTU-3). FC10开销9字节+2N. N=(mtu-12)//2, 保守上限64
    N=max(4,min(64,(mtu-12)//2)); print("每块",N,"寄存器")
    t0=time.time()
    for i in range(0,len(hw),N):
        await c.txn(f10(0x7100,list(hw[i:i+N])),8)
    print("写完 %d寄存器 用时%.1fs"%(len(hw),time.time()-t0))
    pr=await c.rd(0x7002,2);print("已写偏移=",(pr[0]<<16|pr[1]) if pr else None,"(应%d)"%ln)
    print("校验+跳转...");await c.txn(f06(0x7020,2),8,to=2.0);await cl.disconnect();await asyncio.sleep(3.0)
    d=await find();cl=BleakClient(d,timeout=15.0);await cl.connect();c=C(cl);await c.start()
    fw2=await c.rd(0x3E02);print("升级后 app fw=",hex(fw2[0]) if fw2 else None,"← OTA成功" if fw2 else "")
    await cl.disconnect()
asyncio.run(main())
