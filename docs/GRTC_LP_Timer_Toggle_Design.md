# GRTC LP 定时器触发 P0.04 翻转 — 极致低功耗方案

## 1. 功能概述

使用 LP（Low Power）域定时器 GRTC（Global Real-Time Counter）触发 P0.04 引脚翻转：
- **定时间隔**：可调，默认 3s
- **每次触发**：P0.04 翻转 6 次
- **翻转间隔**：可调，默认 300ms
- **功耗**：极致低功耗，仅需 LP 域，无需 HFCLK

## 2. 硬件架构

### 2.1 使用的 LP 域外设

| 外设 | 功能 | 地址 |
|------|------|------|
| **GRTC** | 全局实时计数器，52 位，1µs 分辨率 | 0x400E2000 |
| **GPIOTE30** | GPIO Tasks and Events（LP 域） | 0x4010C000 |
| **DPPIC30** | DPPI 控制器（LP 域） | 0x40102000 |
| **P0** | GPIO 端口 P0（LP 域） | 0x4010A000 |

### 2.2 硬件链路

```
GRTC CC[1]（3s 间隔）
  └─→ 中断 → CPU 启动翻转序列

GRTC CC[0]（300ms，INTERVAL 周期性模式）
  ├─→ DPPI30 CH 0 → GPIOTE30 OUT[0] → P0.04 翻转（硬件自动）
  └─→ 中断 → CPU 计数，第 6 次后停止
```

### 2.3 GRTC 关键特性

- **SYSCOUNTER**：52 位计数器，1µs 分辨率，运行在 LFCLK（32.768kHz）
- **CC[0] 周期性模式**：使用 `INTERVAL` 寄存器，硬件自动将 `CC[0]` 更新为 `CC[0] + INTERVAL`，无需 CPU 干预
- **CC[1] 单次模式**：用于 3s 间隔触发，每次触发后由 CPU 重新设置下一个触发时间
- **低功耗**：SYSCOUNTER 自动进入睡眠模式，仅在需要时唤醒

## 3. 实现细节

### 3.1 初始化流程

1. **配置 GPIOTE30**：
   - 设置 P0.04 为输出，初始低电平
   - 配置 GPIOTE30 CH0 为 Toggle 模式
   - 使能 GPIOTE30 通道

2. **初始化 GRTC**：
   - 启用 SYSCOUNTER（`MODE.SYSCOUNTEREN`）
   - 启动 GRTC（`TASKS_START`）
   - 等待同步事件（`EVENTS_RTCOMPARESYNC`）
   - 配置 `WAKETIME` 和 `TIMEOUT`（用于低功耗模式）

3. **配置 CC[1]（3s 间隔）**：
   - 读取当前 SYSCOUNTER 值
   - 设置 `CC[1] = SYSCOUNTER + 3s`
   - 使能 CC[1] 中断

4. **注册中断**：
   - 注册 GRTC 中断处理函数
   - 使能中断

### 3.2 运行流程

#### 3.2.1 CC[1] 中断（3s 触发）

当 CC[1] 匹配时：
1. 清除事件
2. 如果翻转序列未激活，启动序列：
   - 读取当前 SYSCOUNTER
   - 设置 `CC[0] = SYSCOUNTER + 300ms`
   - 设置 `INTERVAL = 300ms`（启用周期性模式）
   - 启用 CC[0] 通道
   - 配置 DPPI：`GRTC COMPARE[0] → GPIOTE30 OUT[0]`
   - 使能 DPPI 通道
   - 使能 CC[0] 中断
   - 设置 `toggle_sequence_active = true`，`toggle_counter = 0`
3. 设置下一个 3s 触发时间：`CC[1] = SYSCOUNTER + 3s`

#### 3.2.2 CC[0] 中断（300ms 翻转计数）

当 CC[0] 匹配时（硬件已自动翻转 P0.04）：
1. 清除事件
2. `toggle_counter++`
3. 如果 `toggle_counter >= 6`：
   - 禁用 CC[0] 通道
   - 清除 INTERVAL（禁用周期性模式）
   - 禁用 CC[0] 中断
   - 清除 DPPI 连接
   - 禁用 DPPI 通道
   - 设置 `toggle_sequence_active = false`

### 3.3 关键寄存器操作

#### GRTC CC[0] 周期性模式

```c
/* 设置初始比较值 */
GRTC_REG(GRTC_CCL(0)) = (uint32_t)(value & 0xFFFFFFFFUL);
GRTC_REG(GRTC_CCH(0)) = (uint32_t)((value >> 32) & 0xFFFFFUL);

/* 设置 INTERVAL（启用周期性模式） */
GRTC_REG(GRTC_INTERVAL) = 300000UL;  /* 300ms，单位：微秒 */

/* 启用通道 */
GRTC_REG(GRTC_CCEN(0)) = GRTC_CCEN_ACTIVE;
```

#### DPPI 连接

```c
/* GRTC COMPARE[0] 发布到 DPPI30 CH 0 */
GRTC_REG(GRTC_PUBLISH_COMPARE(0)) = 
    GRTC_PUBSUB_EN | (DPPI30_CH_TOGGLE & GRTC_PUBSUB_CHIDX_MASK);

/* GPIOTE30 OUT[0] 订阅 DPPI30 CH 0 */
nrf_gpiote_subscribe_set(NRF_GPIOTE30,
    nrf_gpiote_out_task_get(GPIOTE30_CH), DPPI30_CH_TOGGLE);

/* 使能 DPPI 通道 */
nrf_dppi_channels_enable(NRF_DPPIC30, (1UL << DPPI30_CH_TOGGLE));
```

## 4. 功耗分析

### 4.1 电源域

| 场景 | 必须上电的域 | 说明 |
|------|--------------|------|
| 待机（无翻转） | **LP** | 仅 GRTC 运行，SYSCOUNTER 在睡眠模式 |
| 翻转期间 | **LP** | GRTC、GPIOTE30、DPPI30 活跃 |
| CPU 唤醒 | **LP + MCU** | CPU 仅在中断处理时短暂唤醒（~10µs） |

### 4.2 功耗优势

- **无需 HFCLK**：GRTC 运行在 LFCLK（32.768kHz），功耗极低
- **无需 PERI 域**：不使用 TIMER20/21、GPIOTE20，PERI 域可关闭
- **硬件自动翻转**：CC[0] 周期性模式 + DPPI + GPIOTE30，翻转期间 CPU 仅需计数
- **CPU 唤醒最少**：每 3s 唤醒一次启动序列，每 300ms 唤醒一次计数（共 6 次）

### 4.3 功耗估算

- **待机功耗**：~2.9 µA（System ON IDLE，256KB RAM + GRTC）
- **翻转期间**：LP 域活跃，但无需 HFCLK，功耗仍很低
- **CPU 唤醒**：每次唤醒 ~10µs，功耗可忽略

## 5. 可调参数

### 5.1 定时间隔（默认 3s）

```c
void set_interval_ms(uint32_t ms)
{
    interval_us = ms * 1000UL;
}
```

### 5.2 翻转间隔（默认 300ms）

```c
void set_toggle_interval_ms(uint32_t ms)
{
    toggle_interval_us = ms * 1000UL;
}
```

### 5.3 翻转次数（固定 6 次）

修改 `#define TOGGLE_COUNT` 可改变翻转次数。

## 6. 注意事项

1. **GRTC IRQ**：如果设备树未定义 `grtc` 节点，需手动设置 `GRTC_IRQ` 宏（根据平台确定实际中断号）

2. **SYSCOUNTER 读取**：
   - 必须先设置 `SYSCOUNTER[n].ACTIVE = 1`
   - 等待 `SYSCOUNTERH.BUSY = 0`
   - 先读 `SYSCOUNTERL`，再读 `SYSCOUNTERH`
   - 最后清除 `ACTIVE`

3. **CC 寄存器写入顺序**：
   - 先写 `CCL`（禁用通道）
   - 再写 `CCH`（启用通道）

4. **INTERVAL 寄存器**：
   - 仅适用于 CC[0]
   - 单位：微秒（与 SYSCOUNTER 一致）
   - 设置为 0 可禁用周期性模式

5. **WAKETIME 和 TIMEOUT**：
   - `WAKETIME`：GRTC 在比较事件前提前唤醒的 LFCLK 周期数
   - `TIMEOUT`：SYSCOUNTER 保持活跃状态的 LFCLK 周期数
   - 需满足 `TIMEOUT > WAKETIME + guard_time`

## 7. 与 PERI 域方案对比

| 特性 | PERI 域方案（TIMER20/21 + GPIOTE20） | LP 域方案（GRTC + GPIOTE30） |
|------|-------------------------------------|------------------------------|
| 时钟源 | HFCLK（16MHz） | LFCLK（32.768kHz） |
| 功耗 | 较高（需 HFCLK） | 极低（仅 LFCLK） |
| 电源域 | PERI + MCU | LP（+ MCU 仅中断时） |
| CPU 介入 | 启动/停止链路 | 启动序列 + 计数 |
| 硬件自动翻转 | 是（DPPI） | 是（DPPI + INTERVAL） |

## 8. 总结

本方案实现了使用 LP 域外设的极致低功耗 GPIO 翻转：
- ✅ 仅需 LP 域，无需 PERI 域和 HFCLK
- ✅ 硬件自动周期性翻转（GRTC INTERVAL + DPPI）
- ✅ CPU 介入最少（仅计数和启动序列）
- ✅ 参数可调（定时间隔、翻转间隔）
- ✅ 适用于需要长期运行的超低功耗应用
