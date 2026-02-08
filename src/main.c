/*
 * 极致低功耗 LP 域定时器触发 P0.04 翻转
 *
 * 功能：
 *   GRTC（LP 域全局实时计数器）每 3s 触发一次翻转序列
 *   每次序列：P0.04 每 300ms 翻转一次，共 6 次
 *   定时间隔、翻转间隔、翻转次数均为变量可调
 *
 * 电源域分析（nRF54L15 Datasheet §Power domains）：
 *   ┌─────────┬─────────────────────────────────────┬──────────┐
 *   │ 域      │ 外设                                │ 本方案   │
 *   ├─────────┼─────────────────────────────────────┼──────────┤
 *   │ MCU (0) │ CPU, CACHE                          │ 必须     │
 *   │ RADIO(1)│ RADIO                               │ 不启用   │
 *   │ PERI (2)│ TIMER20-24, GPIOTE20, UART/SPI, P1 │ 不启用   │
 *   │ LP  (3) │ GRTC, P0                            │ ★ 使用   │
 *   └─────────┴─────────────────────────────────────┴──────────┘
 *
 * 功耗优化策略：
 *   - GRTC 运行在 LFCLK（32.768kHz），无需 HFCLK
 *   - P0.04 属于 LP 域，直接 GPIO HAL 翻转
 *   - 空闲时 System ON IDLE（约 2.9µA with GRTC + 256KB RAM）
 *   - 不启用 PERI 域（无 TIMER20/21、GPIOTE20、HFCLK）
 *
 * CPU 介入分析（每 ~4.8s 周期）：
 *   3s 休眠结束 → 中断 1 次（启动翻转序列）
 *   300ms × 6 翻转 → 中断 6 次（GPIO toggle + 计数，~2µs/次）
 *   共 7 次唤醒，总 CPU 时间 ~14µs / 4.8s
 *
 * 注：GRTC 是全局外设（base 0x400E2000），其 PUBLISH 寄存器不直接
 *     连接 DPPIC30，无法用 DPPI30 → GPIOTE30 实现纯硬件翻转。
 *     由于 CPU 已因计数需求而唤醒，ISR 中直接 toggle 是等效最优解。
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#include <hal/nrf_gpio.h>

/* ===== 可配置参数（运行时可调） ===== */
static uint32_t cycle_interval_ms  = 3000;  /* 周期间隔，默认 3s */
static uint32_t toggle_interval_ms = 300;   /* 翻转间隔，默认 300ms */
static uint32_t toggle_count       = 6;     /* 每周期翻转次数 */

/* ===== 引脚定义（LP 域） ===== */
#define OUTPUT_PIN    NRF_GPIO_PIN_MAP(0, 1) /* P0.01，属于 LP 域 */

/* ===== GRTC 频率（SYSCOUNTER 运行在 1 MHz） ===== */
#define GRTC_FREQ_HZ  1000000UL

/* ===== 内部状态 ===== */
static int32_t  grtc_chan;          /* Zephyr 分配的 GRTC CC 通道 */
static volatile uint32_t remaining; /* 剩余翻转次数 */

/* 毫秒转 GRTC ticks（1 MHz → 1 tick = 1µs） */
#define MS_TO_TICKS(ms) ((uint64_t)(ms) * (GRTC_FREQ_HZ / 1000))

/* 前向声明 */
static void on_cycle_compare(int32_t id, uint64_t exp, void *ctx);
static void on_toggle_compare(int32_t id, uint64_t exp, void *ctx);

/* ================================================================
 * on_cycle_compare() — 周期到达，启动翻转序列
 *
 * 3s 定时到期 → 初始化翻转计数 → 设置首次翻转 CC
 * ================================================================ */
static void on_cycle_compare(int32_t id, uint64_t exp, void *ctx)
{
	ARG_UNUSED(id);
	ARG_UNUSED(ctx);

	remaining = toggle_count;

	/* 第一次翻转在 toggle_interval_ms 后 */
	z_nrf_grtc_timer_set(grtc_chan,
		exp + MS_TO_TICKS(toggle_interval_ms),
		on_toggle_compare, NULL);
}

/* ================================================================
 * on_toggle_compare() — 每次翻转的 GRTC compare 回调
 *
 * 每次 ~2µs：GPIO toggle（1 指令）+ 计数 + 设置下次 CC
 * ================================================================ */
static void on_toggle_compare(int32_t id, uint64_t exp, void *ctx)
{
	ARG_UNUSED(id);
	ARG_UNUSED(ctx);

	nrf_gpio_pin_toggle(OUTPUT_PIN);
	remaining--;

	if (remaining > 0) {
		/* 下一次翻转 */
		z_nrf_grtc_timer_set(grtc_chan,
			exp + MS_TO_TICKS(toggle_interval_ms),
			on_toggle_compare, NULL);
	} else {
		/* 翻转序列完成：确保 P0.04 归低（偶数次已为低，奇数次强制拉低） */
		nrf_gpio_pin_clear(OUTPUT_PIN);

		/* 设置下一个周期 */
		z_nrf_grtc_timer_set(grtc_chan,
			exp + MS_TO_TICKS(cycle_interval_ms),
			on_cycle_compare, NULL);
	}
}

/* ================================================================ */
int main(void)
{
	/* P0.04 默认输出低电平 */
	nrf_gpio_cfg_output(OUTPUT_PIN);
	nrf_gpio_pin_clear(OUTPUT_PIN);

	/* 分配 GRTC CC 通道（Zephyr 管理，避免与系统定时器冲突） */
	grtc_chan = z_nrf_grtc_timer_chan_alloc();
	if (grtc_chan < 0) {
		return -1;
	}

	/* 启动第一个周期定时 */
	uint64_t now = z_nrf_grtc_timer_read();

	z_nrf_grtc_timer_set(grtc_chan,
		now + MS_TO_TICKS(cycle_interval_ms),
		on_cycle_compare, NULL);

	/* CPU 进入永久休眠（由 GRTC compare 中断唤醒） */
	k_sleep(K_FOREVER);
	return 0;
}
