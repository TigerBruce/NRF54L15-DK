# 手机 BLE 控制 P0.04 翻转 — 极致低功耗方案

## 1. 目标与约束

- **功能**：手机通过 BLE 写 GATT Characteristic，触发板载 P0.04 翻转（如接 LED）。
- **约束**：极致低功耗、最少电源域、LED 翻转延迟 ≤ 1 秒。

## 2. 关键结论（来自数据手册）

### 2.1 电源域与 GPIO 归属

| 域       | 典型外设     | GPIO 端口 | GPIOTE   |
|----------|--------------|-----------|----------|
| MCU (0)  | CPU, CACHE   | **P2**    | —        |
| RADIO (1)| RADIO        | —         | —        |
| PERI (2) | TIMER20–24, GPIOTE20, UART/SPI 等 | **P1** | GPIOTE20 |
| **LP (3)** | GRTC, COMP, **P0**, **GPIOTE30** | **P0** | **GPIOTE30** |

- **P0.04 = Port P0 Pin 4，属于 LP 域。**
- 控制 P0.04 应使用 **LP 域的 P0 + GPIOTE30**，**不需要开启 PERI 域**（不碰 P1/GPIOTE20/TIMER20 等）。

### 2.2 功耗参考（数据手册）

- System ON IDLE（256KB RAM + GRTC）：约 **2.9 µA**
- System OFF：约 **0.7 µA**（无 GRTC 唤醒时）
- BLE 连接时：取决于连接间隔与空中时间；连接间隔拉长可显著降低平均电流。

## 3. 方案设计

### 3.1 架构

```
手机 App (写 GATT) → BLE 链路 → 从机 GATT 写回调 → 翻转 P0.04 (LP 域)
```

- **BLE**：从机，单 Service + 单 Characteristic（Write / Write Without Response）。
- **GPIO**：仅操作 **P0.04**，通过 **LP 域**（P0 寄存器或 GPIOTE30）翻转，**不启用 PERI 域**。

### 3.2 电源域策略（最少域）

| 场景           | 必须上电的域 | 说明 |
|----------------|--------------|------|
| 待机/未连接    | 无 或 LP(GRTC) | 若需 RTC 可保留 GRTC（LP 域） |
| 广播/连接      | MCU + RADIO  | 协议栈与链路层必需 |
| 收到写并翻转 IO | MCU + RADIO + **LP** | 仅用 P0/GPIOTE30，**不必开 PERI** |

要点：  
- 应用层**不要**使用 P1、GPIOTE20、TIMER20/21 等 PERI 外设，则 PERI 域可在连接期间由系统按需关闭或保持关闭（视 Zephyr/NCS 的电源管理策略而定）。  
- P0.04 的翻转全部在 **LP 域** 完成，实现「最少电源域」。

### 3.3 满足「1 秒内翻转」的连接参数

- **Connection Interval**：建议 **800 ms～1000 ms**（BLE 允许上限约 4000 ms，取 1s 兼顾延迟与功耗）。
- **Slave Latency**：可设 **4～9**（从机可连续跳过 4～9 个连接事件再唤醒），这样平均延迟仍在 1 秒内，且进一步降低功耗。
- 手机写 GATT 后，从机最晚在下一个连接事件即可处理，**典型延迟 < 1 秒**。

### 3.4 翻转实现方式（LP 域）

- **推荐**：直接用 **HAL 写 P0**（`nrf_gpio` 或 nRF54L 的 P0 寄存器），或使用 **GPIOTE30** 的 Toggle Task。  
- 避免使用 Zephyr 的 `gpio1`（P1）或任何 PERI 域外设。  
- 若 Zephyr 的 `gpio0` 在 nRF54L 上对应 P0（LP 域），也可用 `gpio_pin_toggle_dt(&led_p0_04)`，需在 DTS 中把 `led0`/自定义别名指向 P0.04，并确认不会拉高 PERI。

## 4. 实现要点（最小改动）

### 4.1 工程配置

- **prj.conf**（BLE 构建）：  
  - 启用 BLE 从机、GATT 服务器、自定义 Service。  
  - 关闭不需要的外设（如 UART 控制台、其他 PERI 外设）。  
  - 连接参数示例（需与 NCS/Zephyr 选项一致）：
    - `CONFIG_BT_CONN_PARAM_UPDATE=y`，并在代码中请求 `interval_min/interval_max = 800～1000`（单位 1.25 ms），`latency = 4～9`。  
  - 已提供 **boards/nrf54l15dk_nrf54l15_cpuapp_ble_p0.overlay**，供 BLE + P0.04 构建使用（不关 RADIO、不启用 PERI 的 TIMER20/21/GPIOTE20）。

- **Overlay**：  
  - 使用**不带「关闭 RADIO」** 的 overlay（与当前按钮低功耗 demo 的 overlay 区分开）。  
  - 为 P0.04 增加 `gpio0` 的 alias（若用 DTS 控制 LED）：已在 **ble_p0.overlay** 中定义 `led_p0_04` 与 `led-p0-04`。  
  - 不启用 TIMER20/21、GPIOTE20 等 PERI 外设，避免 PERI 域被拉起来。

### 4.2 代码结构（简要）

1. **初始化**  
   - 配置 P0.04 为输出（通过 LP 域：直接 P0 或 GPIOTE30），初始电平一致（如低）。

2. **GATT**  
   - 注册自定义 Service + Characteristic（Write / Write Without Response）。  
   - 在 CCC 或 Write 回调里：解析「写请求」→ 调用「翻转 P0.04」函数。

3. **翻转 P0.04**  
   - 使用 `NRF_GPIO_PIN_MAP(0, 4)` 或等效，通过 HAL 写 P0 OUT 寄存器取反，或触发 GPIOTE30 的 Toggle Task。  
   - 不再使用 P1.10（当前 main.c 的 LED1）和 TIMER20/21/DPPI 链路，以保持 PERI 域关闭。

4. **连接参数**  
   - 在 `connected` 或 `connection_param` 回调中请求：  
     - min/max interval = 800～1000（1.25 ms 单位），latency = 4～9，supervision_timeout 适当（如 4000～6000 ms）。

### 4.3 与现有 main.c 的关系

- 当前 **main.c** 是「按钮 + TIMER20/21 + DPPI + P1.10」的极致低功耗 demo，**无 BLE**。  
- 建议：  
  - **方案 A**：新建 `main_ble_gpio.c`（或子目录 sample）专门做「BLE 写 → P0.04 翻转」，用单独 overlay + prj.conf 构建，与现有按钮 demo 并存。  
  - **方案 B**：在现有工程中通过 Kconfig/overlay 切换「按钮 demo」与「BLE GPIO demo」，避免两套逻辑混在同一 main 里，保持函数与文件长度可控。

## 5. 潜在问题与建议

1. **板型与 overlay**：当前提供的 overlay 为 **nrf54l15dk_nrf54l15_cpuapp_ble_p0.overlay**（适用于 nRF54L15 DK）。若使用 **PCA10156** 或其他板型，需选用对应 board 的 target，并将 overlay 复制/改名为该板型的 `boards/<board>_ble_p0.overlay`，或在构建时通过 `-DOVERLAY_FILE=...` 指定。  
2. **PCA10156 原理图**：确认 P0.04 在板子上是否已引出、是否接 LED 或预留 IO。若未引出，需改硬件或换用同属 LP 域的其他 P0.x。  
3. **Zephyr/NCS 默认电源管理**：协议栈或驱动可能在连接时默认开启 PERI。若实测 PERI 仍被上电，需在 NCS 中查「power domain」或「peripheral domain」相关 Kconfig，关闭非必要 PERI 外设。  
4. **GRTC**：若希望待机时用 System OFF + GRTC 唤醒，需保留 GRTC（LP 域）；若仅做「连接时低功耗」，可先不纠结 System OFF。  
5. **延迟与功耗折中**：1 秒延迟已很宽松，连接间隔 1000 ms + Slave Latency 4～9 即可；若希望更快响应（如 200 ms），可适当减小 interval 与 latency，功耗会略升。

## 6. 代码要点（LP 域翻转 P0.04）

- **引脚**：`NRF_GPIO_PIN_MAP(0, 4)` 即 P0.04（LP 域）。  
- **翻转**（HAL，不依赖 PERI）：
  - 方式一：`nrf_gpio_cfg_output(pin);` 初始化一次，之后 `nrf_gpio_pin_toggle(pin);`。  
  - 方式二：用 GPIOTE30 的 Toggle Task + DPPI（若希望完全硬件链式可扩展）。  
- **GATT 写回调**：在 `bt_gatt_attr_write` 或 NCS 的 `bt_gatt_cb.att_write` 里解析 len/data，调用上述翻转函数即可。  
- **连接参数**：在 `connected` 回调里调用 `bt_conn_param_update(conn, &param)`，其中 `param.interval_min = 800`，`param.interval_max = 1000`（单位 1.25 ms），`param.latency = 4`，`param.timeout = 40`（单位 10 ms）。

## 7. 小结

- **P0.04 在 LP 域**，用 **P0 寄存器或 GPIOTE30** 翻转，**不依赖 PERI 域**，实现最少电源域。  
- **BLE**：GATT Write 触发翻转；连接参数 800～1000 ms + Slave Latency 4～9，满足 1 秒内延迟并利于低功耗。  
- 与现有「按钮 + TIMER/DPPI + P1.10」的 demo 分离构建（不同 main 或 overlay），避免混用 PERI 域，便于维护与审查。
