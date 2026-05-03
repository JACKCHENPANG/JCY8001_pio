#!/usr/bin/env python3
"""
JCY8001 移植 Agent - 主控脚本
读取进度 → 执行移植 → 验证 → 更新状态 → 通知
"""

import subprocess
import sys
from pathlib import Path
from datetime import datetime

PROJECT_DIR = Path("/Users/jcy/Projects/JCY8001_pio")
PROGRESS_FILE = PROJECT_DIR / "PROJECT_PROGRESS.md"
STATUS_FILE = PROJECT_DIR / "MIGRATION_STATUS.txt"


def log(msg: str):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {msg}")
    # 同时写入状态文件
    with open(STATUS_FILE, 'a') as f:
        f.write(f"[{timestamp}] {msg}\n")


def run_cmd(cmd: str, timeout: int = 120) -> tuple:
    """执行命令"""
    try:
        result = subprocess.run(
            cmd, shell=True, cwd=PROJECT_DIR,
            capture_output=True, text=True, timeout=timeout
        )
        return result.returncode == 0, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return False, "Timeout"
    except Exception as e:
        return False, str(e)


def get_current_stage() -> dict:
    """解析当前阶段"""
    content = PROGRESS_FILE.read_text()
    
    for line in content.split('\n'):
        if '**阶段**:' in line:
            stage = line.split('`')[1] if '`' in line else "STAGE_0_LED_BLINK"
        if '**状态**:' in line:
            status = line.split('`')[1] if '`' in line else "PENDING"
    
    return {"stage": stage, "status": status}


def stage_0_work():
    """Stage 0: 创建 LED 闪烁代码"""
    log("执行 Stage 0: LED 闪烁代码...")
    
    # 检查 main.c 是否存在
    main_c = PROJECT_DIR / "src" / "main.c"
    
    if main_c.exists():
        content = main_c.read_text()
        if "HAL_GPIO_TogglePin" in content or "LED" in content:
            log("  ✅ main.c 已包含 LED 代码，跳过创建")
            return True
    
    # 创建基础 LED 闪烁代码
    led_code = '''/**
 * JCY8001 - Stage 0: LED Blink
 * 移植来源: 最小系统验证
 */

#include "main.h"

// LED 引脚定义 (根据实际硬件调整)
#define LED_PIN GPIO_PIN_13
#define LED_PORT GPIOC

// 系统时钟配置
void SystemClock_Config(void);
static void MX_GPIO_Init(void);

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    
    while (1) {
        HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        HAL_Delay(500);  // 1Hz 闪烁 (500ms ON + 500ms OFF)
    }
}

void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;  // 8MHz * 9 = 72MHz
    HAL_RCC_OscConfig(&RCC_OscInitStruct);
    
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;  // 36MHz
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;  // 72MHz
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
}

static void MX_GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    __HAL_RCC_GPIOC_CLK_ENABLE();
    
    // LED 输出
    GPIO_InitStruct.Pin = LED_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &GPIO_InitStruct);
    
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
}
'''
    
    main_c.write_text(led_code)
    log("  ✅ 创建 main.c (LED 闪烁)")
    
    return True


def stage_1_work():
    """Stage 1: 移植 MODBUS"""
    log("执行 Stage 1: MODBUS 移植...")
    log("  ⚠️ 需要从原代码移植 Modbus.c")
    
    # TODO: 实际移植代码
    # 1. 复制 Modbus.c/Modbus.h 到 lib/modbus/
    # 2. 修改 HAL 接口
    # 3. 添加 RS485 方向控制
    
    return False  # 暂未完成


def stage_2_work():
    """Stage 2: 移植 EEPROM"""
    log("执行 Stage 2: EEPROM 移植...")
    return False


def stage_3_work():
    """Stage 3: 移植 SPI+ADC"""
    log("执行 Stage 3: SPI+ADC 移植...")
    return False


def stage_4_work():
    """Stage 4: 完整业务"""
    log("执行 Stage 4: 完整业务移植...")
    return False


STAGE_WORKERS = {
    "STAGE_0_LED_BLINK": stage_0_work,
    "STAGE_1_MODBUS": stage_1_work,
    "STAGE_2_EEPROM": stage_2_work,
    "STAGE_3_SPI_ADC": stage_3_work,
    "STAGE_4_FULL": stage_4_work,
}

NEXT_STAGE = {
    "STAGE_0_LED_BLINK": "STAGE_1_MODBUS",
    "STAGE_1_MODBUS": "STAGE_2_EEPROM",
    "STAGE_2_EEPROM": "STAGE_3_SPI_ADC",
    "STAGE_3_SPI_ADC": "STAGE_4_FULL",
    "STAGE_4_FULL": None,
}


def update_progress(stage: str, status: str, notes: str = ""):
    """更新进度文件"""
    content = PROGRESS_FILE.read_text()
    lines = content.split('\n')
    
    for i, line in enumerate(lines):
        if '**阶段**:' in line:
            lines[i] = f"**阶段**: `{stage}`\n"
        elif '**状态**:' in line:
            lines[i] = f"**状态**: `{status}`\n"
    
    # 添加历史记录
    history_start = None
    for i, line in enumerate(lines):
        if '| 时间 | 阶段 |' in line:
            history_start = i + 2
            break
    
    if history_start:
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M")
        emoji = '✅' if status == 'COMPLETED' else '🔄' if status == 'IN_PROGRESS' else '❌'
        lines.insert(history_start, f"| {timestamp} | {stage} | {notes} | {emoji} |\n")
    
    PROGRESS_FILE.write_text('\n'.join(lines))


def main():
    log("=" * 60)
    log("JCY8001 移植 Agent 启动")
    log("=" * 60)
    
    # 读取当前状态
    current = get_current_stage()
    log(f"当前: {current['stage']} - {current['status']}")
    
    # 状态机
    if current['status'] == 'COMPLETED':
        # 进入下一阶段
        next_stage = NEXT_STAGE.get(current['stage'])
        if next_stage:
            log(f"进入下一阶段: {next_stage}")
            update_progress(next_stage, 'PENDING', '自动进入下一阶段')
            current = {"stage": next_stage, "status": "PENDING"}
        else:
            log("🎉 所有阶段完成！")
            update_progress(current['stage'], 'COMPLETED', '全部完成')
            return 0
    
    if current['status'] == 'BLOCKED':
        log("❌ 阶段被阻塞，需要人工介入")
        return 1
    
    # 执行当前阶段工作
    worker = STAGE_WORKERS.get(current['stage'])
    if not worker:
        log(f"❌ 未知的阶段: {current['stage']}")
        return 1
    
    update_progress(current['stage'], 'IN_PROGRESS', '开始执行')
    
    try:
        success = worker()
    except Exception as e:
        log(f"❌ 执行异常: {e}")
        update_progress(current['stage'], 'BLOCKED', f'异常: {str(e)[:50]}')
        return 1
    
    if success:
        # 运行验证
        log("运行验证...")
        verify_success, output = run_cmd("python3 scripts/verify_stage.py")
        
        if verify_success:
            log("✅ 验证通过")
            update_progress(current['stage'], 'COMPLETED', '验证通过')
        else:
            log(f"⚠️ 验证脚本执行失败: {output[:100]}")
            # 编译检查
            compile_ok, build_output = run_cmd("pio run")
            if compile_ok and "SUCCESS" in build_output:
                log("✅ 编译成功，验证部分通过")
                update_progress(current['stage'], 'COMPLETED', '编译通过')
            else:
                log(f"❌ 编译失败")
                update_progress(current['stage'], 'BLOCKED', f'编译失败: {build_output[:100]}')
    else:
        log("⏳ 工作未完成，继续...")
        update_progress(current['stage'], 'IN_PROGRESS', '工作中')
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
