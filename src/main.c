/*
 * main.c - JCY8001 Firmware (PlatformIO 移植版)
 *
 * 基于 jcy8001_firmware v0.6 移植到 PlatformIO
 */

#include "usart.h"
#include "spi.h"
#include "modbus.h"
#include "register.h"

extern void SystemInit(void);

int main(void) {
    /* 初始化寄存器 */
    register_init();

    /* 初始化 USART2 (Modbus 通讯) */
    usart2_init(115200);

    /* 主循环 */
    while (1) {
        /* 处理 Modbus 帧 */
        modbus_poll();
    }

    return 0;
}
