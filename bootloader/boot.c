#include <stdint.h>

/* ── 裸机寄存器 ── */
#define REG(a) (*(volatile uint32_t*)(a))
#define RCC_APB2ENR REG(0x40021018u)
#define RCC_APB1ENR REG(0x4002101Cu)
#define GPIOA_CRL   REG(0x40010800u)
#define US2_SR      REG(0x40004400u)
#define US2_DR      REG(0x40004404u)
#define US2_BRR     REG(0x40004408u)
#define US2_CR1     REG(0x4000440Cu)
#define GPIOA_CRH   REG(0x40010804u)
#define US1_SR      REG(0x40013800u)   /* USART1 (PA9/PA10) → JDY-10 蓝牙 */
#define US1_DR      REG(0x40013804u)
#define US1_BRR     REG(0x40013808u)
#define US1_CR1     REG(0x4001380Cu)
#define FLASH_KEYR  REG(0x40022004u)
#define FLASH_SR    REG(0x4002200Cu)
#define FLASH_CR    REG(0x40022010u)
#define FLASH_AR    REG(0x40022014u)
#define SCB_VTOR    REG(0xE000ED08u)
#define SCB_AIRCR   REG(0xE000ED0Cu)
#define PWR_CR      REG(0x40007000u)
#define BKP_DR1     REG(0x40006C04u)
#define BKP_MAGIC   0xB007u
#define FL_BSY  (1u<<0)
#define FL_PG   (1u<<0)
#define FL_PER  (1u<<1)
#define FL_STRT (1u<<6)
#define FL_LOCK (1u<<7)

#define APP_ADDR     0x08004000u
#define APP_END      0x0803F800u      /* 标定数据页之前 */
#define PAGE_SZ      0x800u           /* 2KB/页 (F103RC 高密度) */


extern uint32_t _sidata,_sdata,_edata,_sbss,_ebss,_estack;
int boot_main(void);
void Reset_Handler_impl(void);
void Default_Handler(void){ while(1){} }

__attribute__((used,section(".isr_vector")))
void(*const g_vec[])(void)={
  (void(*)(void))(&_estack), Reset_Handler_impl, Default_Handler, Default_Handler,
  Default_Handler,Default_Handler,Default_Handler,0,0,0,0,
  Default_Handler,Default_Handler,0,Default_Handler,Default_Handler,
};
void Reset_Handler_impl(void){
  uint32_t *s=&_sidata,*d=&_sdata;
  while(d<&_edata)*d++=*s++;
  for(d=&_sbss;d<&_ebss;)*d++=0;
  boot_main();
  while(1){}
}

/* ── 双串口 115200 @ HSI 8MHz: USART2(CP2102 USB有线) + USART1(JDY-10 蓝牙) ── */
static int cur_port = 2;   /* 当前处理/回复的端口: 2=USB有线, 1=蓝牙 */
static void uart_init(void){
  RCC_APB2ENR |= (1u<<0)|(1u<<2)|(1u<<14);   /* AFIOEN | IOPAEN | USART1EN */
  RCC_APB1ENR |= (1u<<17);                   /* USART2EN */
  GPIOA_CRL = (GPIOA_CRL & 0xFFFF0000u) | 0x00004B04u;   /* PA2 TX-AF / PA3 RX (USART2) */
  GPIOA_CRH = (GPIOA_CRH & ~0x00000FF0u) | 0x000004B0u;  /* PA9 TX-AF / PA10 RX (USART1) */
  US2_BRR = 0x0045u; US2_CR1 = (1u<<13)|(1u<<3)|(1u<<2); /* UE|TE|RE */
  US1_BRR = 0x0045u; US1_CR1 = (1u<<13)|(1u<<3)|(1u<<2);
}
static int u2_ready(void){ return (US2_SR & (1u<<5))!=0; }
static int u1_ready(void){ return (US1_SR & (1u<<5))!=0; }
static void uart_tx(uint8_t b){
  if(cur_port==1){ while(!(US1_SR&(1u<<7))){} US1_DR=b; }
  else           { while(!(US2_SR&(1u<<7))){} US2_DR=b; }
}

/* ── Flash 驱动 ── */
static void fl_unlock(void){ FLASH_KEYR=0x45670123u; FLASH_KEYR=0xCDEF89ABu; }
static void fl_lock(void){ FLASH_CR|=FL_LOCK; }
static void fl_wait(void){ while(FLASH_SR&FL_BSY){} }
static void fl_erase_page(uint32_t addr){
  fl_wait(); FLASH_CR|=FL_PER; FLASH_AR=addr; FLASH_CR|=FL_STRT; fl_wait(); FLASH_CR&=~FL_PER;
}
static void fl_prog_hw(uint32_t addr,uint16_t hw){
  fl_wait(); FLASH_CR|=FL_PG; *(volatile uint16_t*)addr=hw; fl_wait(); FLASH_CR&=~FL_PG;
}

/* ── CRC32 (zlib/反射多项式, 与 python zlib.crc32 一致) ── */
static uint32_t crc32_buf(const uint8_t*p,uint32_t n){
  uint32_t c=0xFFFFFFFFu;
  for(uint32_t i=0;i<n;i++){ c^=p[i]; for(int k=0;k<8;k++) c=(c>>1)^(0xEDB88320u&(-(int)(c&1))); }
  return ~c;
}
/* ── Modbus CRC16 ── */
static uint16_t crc16(const uint8_t*d,uint16_t n){
  uint16_t c=0xFFFF;
  for(uint16_t i=0;i<n;i++){ c^=d[i]; for(int k=0;k<8;k++) c=(c&1)?((c>>1)^0xA001):(c>>1); }
  return c;
}

/* ── 升级状态 ── */
static uint32_t up_len=0, up_crc=0, up_off=0;
static uint16_t up_status=0;   /* 0 idle,1 erased,2 writing,3 verified-ok,0xE error */

static int app_valid(void){
  uint32_t sp=*(volatile uint32_t*)APP_ADDR, pc=*(volatile uint32_t*)(APP_ADDR+4);
  if(sp<0x20000000u || sp>0x2000C000u) return 0;
  if(pc<APP_ADDR || pc>=APP_END) return 0;
  return 1;
}
static void jump_app(void){
  uint32_t sp=*(volatile uint32_t*)APP_ADDR, pc=*(volatile uint32_t*)(APP_ADDR+4);
  SCB_VTOR=APP_ADDR;
  __asm volatile("msr msp,%0"::"r"(sp));
  ((void(*)(void))pc)();
}

static void modbus_reply(uint8_t*d,uint16_t n){
  uint16_t c=crc16(d,n); for(uint16_t i=0;i<n;i++) uart_tx(d[i]); uart_tx(c&0xFF); uart_tx(c>>8);
}

static void handle(uint8_t*rx,uint16_t n){
  if(n<4) return;
  if(rx[0]!=1) return;
  if(crc16(rx,n-2)!=((uint16_t)rx[n-1]<<8|rx[n-2])) return;
  uint8_t fc=rx[1]; uint16_t addr=(rx[2]<<8)|rx[3];
  if(fc==0x03){
    uint16_t cnt=(rx[4]<<8)|rx[5]; if(cnt>32)cnt=32;
    uint8_t tx[5+64]; tx[0]=1; tx[1]=3; tx[2]=cnt*2;
    for(uint16_t i=0;i<cnt;i++){
      uint16_t a=addr+i,v=0;
      if(a==0x7000)v=0xB007; else if(a==0x7001)v=up_status;
      else if(a==0x7002)v=(uint16_t)(up_off>>16); else if(a==0x7003)v=(uint16_t)up_off;
      tx[3+i*2]=v>>8; tx[4+i*2]=v;
    }
    modbus_reply(tx,3+cnt*2);
  } else if(fc==0x06){
    uint16_t v=(rx[4]<<8)|rx[5];
    if(addr==0x7020){
      if(v==1){ /* 擦除 App 区 (按长度算页数) */
        fl_unlock();
        uint32_t pages=(up_len+PAGE_SZ-1)/PAGE_SZ; if(pages==0)pages=1;
        for(uint32_t i=0;i<pages;i++){ uint32_t pa=APP_ADDR+i*PAGE_SZ; if(pa>=APP_END)break; fl_erase_page(pa); }
        fl_lock(); up_off=0; up_status=1;
      } else if(v==2){ /* 完成: 校验 CRC32 */
        uint32_t c=crc32_buf((const uint8_t*)APP_ADDR,up_len);
        if(up_len>0 && c==up_crc){ up_status=3; modbus_reply(rx,6);
          for(volatile int i=0;i<200000;i++){} jump_app(); /* 验证通过 → 跳 App */
          return;
        } else up_status=0xE;
      }
    }
    modbus_reply(rx,6);
  } else if(fc==0x10){
    uint16_t cnt=(rx[4]<<8)|rx[5]; uint8_t*data=&rx[7];
    if(addr==0x7010 && cnt>=4){ /* 设长度+CRC32 */
      up_len=((uint32_t)data[0]<<24)|(data[1]<<16)|(data[2]<<8)|data[3];
      up_crc=((uint32_t)data[4]<<24)|(data[5]<<16)|(data[6]<<8)|data[7];
      up_off=0; up_status=0;
    } else if(addr==0x7100){ /* 数据块: 每寄存器=小端半字 写 Flash */
      fl_unlock();
      for(uint16_t i=0;i<cnt;i++){
        uint16_t hw=((uint16_t)data[i*2]<<8)|data[i*2+1]; /* Modbus hi|lo = 小端半字内容 */
        uint32_t a=APP_ADDR+up_off;
        if(a+1<APP_END){ fl_prog_hw(a,hw); up_off+=2; }
      }
      fl_lock(); up_status=2;
    }
    uint8_t tx[6]; tx[0]=1;tx[1]=0x10;tx[2]=rx[2];tx[3]=rx[3];tx[4]=rx[4];tx[5]=rx[5];
    modbus_reply(tx,6);
  }
}

int boot_main(void){
  /* 检查 BKP 升级标志 (系统复位不丢) */
  RCC_APB1ENR |= (1u<<28)|(1u<<27);   /* PWREN | BKPEN */
  PWR_CR |= (1u<<8);                  /* DBP: 解锁备份域写 */
  int force_update = ((BKP_DR1 & 0xFFFFu)==BKP_MAGIC);
  if(force_update) BKP_DR1=0;         /* 清标志 */
  if(!force_update && app_valid()) jump_app();   /* 正常: 跳 App */
  /* 否则进升级模式: 同时监听 USB(USART2) 和 蓝牙(USART1), 谁来帧从谁回 */
  uart_init();
  static uint8_t rx2[256], rx1[256];
  uint16_t idx2=0, idx1=0; uint32_t idle2=0, idle1=0;
  while(1){
    if(u2_ready()){ if(idx2<sizeof(rx2)) rx2[idx2++]=(uint8_t)US2_DR; idle2=0; }
    else { if(idx2>=4 && ++idle2>20000){ cur_port=2; handle(rx2,idx2); idx2=0; idle2=0; } }
    if(u1_ready()){ if(idx1<sizeof(rx1)) rx1[idx1++]=(uint8_t)US1_DR; idle1=0; }
    else { if(idx1>=4 && ++idle1>20000){ cur_port=1; handle(rx1,idx1); idx1=0; idle1=0; } }
  }
}
