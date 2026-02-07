/*
 * 极致低功耗按钮触发 IO 翻转 — 功耗优化版
 *
 * 功能：
 *   按下 BUTTON0 → LED1 每 500ms 翻转一次，共 6 次
 *
 * 功耗优化策略：
 *   空闲时不配置 TIMER/GPIOTE → HFCLK 自动关闭 → 极低功耗
 *   按钮按下（CPU 介入第 1 次）→ 配置外设 + 启动硬件链路
 *   链路完成（CPU 介入第 2 次）→ TIMER21 中断 → 关闭外设 → HFCLK 关闭
 *   每次 CPU 介入 ~10µs，其余时间深度休眠
 *
 * 硬件链路（仅在翻转期间运行）：
 *   TIMER20（500ms）─ DPPI CH 8 ─┬→ GPIOTE20 CH0 翻转 LED1
 *                                 └→ TIMER21 COUNT +1
 *   TIMER21（CC=6） ─ DPPI CH 9 ─┬→ TIMER20 STOP
 *                                 └→ TIMER21 STOP → 触发中断 → 清理
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>

#include <hal/nrf_timer.h>
#include <hal/nrf_gpiote.h>
#include <hal/nrf_dppi.h>
#include <hal/nrf_gpio.h>

/* ===== 按钮 ===== */
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* ===== LED0：状态指示（Zephyr GPIO） ===== */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ===== LED1：翻转输出（纯 HAL） ===== */
#define OUTPUT_PIN  NRF_GPIO_PIN_MAP(1, 10)
#define GPIOTE_CH   0

/* ===== DPPI 通道 ===== */
#define DPPI_CH_TOGGLE  8
#define DPPI_CH_STOP    9

/* ===== TIMER21 中断 ===== */
#define TIMER21_IRQ  DT_IRQN(DT_NODELABEL(timer21))

/* 链路运行状态 */
static volatile bool chain_active;

/* ================================================================
 * cleanup_chain() — 关闭所有外设，释放 HFCLK
 * 在 TIMER21 中断中调用，也可在重新启动前调用
 * ================================================================ */
static void cleanup_chain(void)
{
	/* 停止定时器 */
	nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(NRF_TIMER21, NRF_TIMER_TASK_STOP);

	/* 禁用 TIMER21 COMPARE 中断 */
	nrf_timer_int_disable(NRF_TIMER21, NRF_TIMER_INT_COMPARE0_MASK);

	/* 禁用 DPPI 通道 */
	nrf_dppi_channels_disable(NRF_DPPIC20,
		(1UL << DPPI_CH_TOGGLE) | (1UL << DPPI_CH_STOP));

	/* 清除所有 DPPI 连接 */
	nrf_timer_publish_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);
	nrf_gpiote_subscribe_clear(NRF_GPIOTE20,
		nrf_gpiote_out_task_get(GPIOTE_CH));
	nrf_timer_subscribe_clear(NRF_TIMER21, NRF_TIMER_TASK_COUNT);
	nrf_timer_publish_clear(NRF_TIMER21, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_subscribe_clear(NRF_TIMER20, NRF_TIMER_TASK_STOP);
	nrf_timer_subscribe_clear(NRF_TIMER21, NRF_TIMER_TASK_STOP);

	/* 禁用 GPIOTE 通道 → LED1 引脚回归 GPIO 控制 */
	nrf_gpiote_task_disable(NRF_GPIOTE20, GPIOTE_CH);

	/* 禁用 TIMER20 SHORTS */
	nrf_timer_shorts_disable(NRF_TIMER20,
		NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK);

	/*
	 * 将 TIMER20 改为 Counter 模式（不请求 HFCLK）
	 * Timer 模式需要 HFCLK 驱动，即使 STOP 也可能保持时钟域活跃
	 * Counter 模式不使用 HFCLK，可安全释放
	 */
	nrf_timer_mode_set(NRF_TIMER20, NRF_TIMER_MODE_COUNTER);

	/* 确保 LED1 灭 */
	nrf_gpio_cfg_output(OUTPUT_PIN);
	nrf_gpio_pin_clear(OUTPUT_PIN);

	chain_active = false;
}

/* ================================================================
 * TIMER21 COMPARE[0] 中断 — 链路完成，清理外设释放 HFCLK
 * ================================================================ */
static void timer21_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (nrf_timer_event_check(NRF_TIMER21, NRF_TIMER_EVENT_COMPARE0)) {
		nrf_timer_event_clear(NRF_TIMER21, NRF_TIMER_EVENT_COMPARE0);
		cleanup_chain();
	}
}

/* ================================================================
 * setup_and_start_chain() — 完整配置外设 + 启动翻转链路
 * CPU 介入第 1 次：按钮中断中调用
 * ================================================================ */
static void setup_and_start_chain(void)
{
	/* 如果链路正在运行，先清理 */
	if (chain_active) {
		cleanup_chain();
	}

	/* --- TIMER20：定时器模式，500ms --- */
	nrf_timer_mode_set(NRF_TIMER20, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(NRF_TIMER20, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(NRF_TIMER20, NRF_TIMER_FREQ_1MHz);
	nrf_timer_cc_set(NRF_TIMER20, NRF_TIMER_CC_CHANNEL0, 500000);
	nrf_timer_shorts_enable(NRF_TIMER20,
		NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK);
	nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_CLEAR);
	nrf_timer_event_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);

	/* --- TIMER21：计数器模式，CC=6 --- */
	nrf_timer_mode_set(NRF_TIMER21, NRF_TIMER_MODE_COUNTER);
	nrf_timer_bit_width_set(NRF_TIMER21, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_cc_set(NRF_TIMER21, NRF_TIMER_CC_CHANNEL0, 6);
	nrf_timer_task_trigger(NRF_TIMER21, NRF_TIMER_TASK_CLEAR);
	nrf_timer_event_clear(NRF_TIMER21, NRF_TIMER_EVENT_COMPARE0);

	/* --- GPIOTE20 CH0：LED1 翻转输出 --- */
	nrf_gpio_cfg_output(OUTPUT_PIN);
	nrf_gpio_pin_clear(OUTPUT_PIN);
	nrf_gpiote_task_configure(NRF_GPIOTE20, GPIOTE_CH, OUTPUT_PIN,
		NRF_GPIOTE_POLARITY_TOGGLE, NRF_GPIOTE_INITIAL_VALUE_LOW);
	nrf_gpiote_task_enable(NRF_GPIOTE20, GPIOTE_CH);

	/* --- DPPI 通道 8：TIMER20 COMPARE → 翻转 + 计数 --- */
	nrf_timer_publish_set(NRF_TIMER20,
		NRF_TIMER_EVENT_COMPARE0, DPPI_CH_TOGGLE);
	nrf_gpiote_subscribe_set(NRF_GPIOTE20,
		nrf_gpiote_out_task_get(GPIOTE_CH), DPPI_CH_TOGGLE);
	nrf_timer_subscribe_set(NRF_TIMER21,
		NRF_TIMER_TASK_COUNT, DPPI_CH_TOGGLE);

	/* --- DPPI 通道 9：TIMER21 COMPARE → 停止链路 --- */
	nrf_timer_publish_set(NRF_TIMER21,
		NRF_TIMER_EVENT_COMPARE0, DPPI_CH_STOP);
	nrf_timer_subscribe_set(NRF_TIMER20,
		NRF_TIMER_TASK_STOP, DPPI_CH_STOP);
	nrf_timer_subscribe_set(NRF_TIMER21,
		NRF_TIMER_TASK_STOP, DPPI_CH_STOP);

	/* --- 使能 DPPI 通道 --- */
	nrf_dppi_channels_enable(NRF_DPPIC20,
		(1UL << DPPI_CH_TOGGLE) | (1UL << DPPI_CH_STOP));

	/* --- 使能 TIMER21 COMPARE 中断（链路完成时清理） --- */
	nrf_timer_int_enable(NRF_TIMER21, NRF_TIMER_INT_COMPARE0_MASK);

	chain_active = true;

	/* --- 启动 --- */
	nrf_timer_task_trigger(NRF_TIMER21, NRF_TIMER_TASK_START);
	nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_START);
}

/* ================================================================
 * 按钮中断回调
 * ================================================================ */
static volatile int64_t last_press_time;

static void button_pressed(const struct device *dev,
			   struct gpio_callback *cb, uint32_t pins)
{
	int64_t now = k_uptime_get();

	if ((now - last_press_time) < 300) {
		return;
	}
	last_press_time = now;

	gpio_pin_toggle_dt(&led0);
	setup_and_start_chain();
}

/* ================================================================ */
int main(void)
{
	/* LED0 初始化 */
	gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);

	/* LED0 亮 1 秒 → 程序启动 */
	gpio_pin_set_dt(&led0, 1);
	k_msleep(1000);
	gpio_pin_set_dt(&led0, 0);

	/* 注册 TIMER21 中断（链路完成时清理外设） */
	IRQ_CONNECT(TIMER21_IRQ, 1, timer21_isr, NULL, 0);
	irq_enable(TIMER21_IRQ);

	/* 配置按钮 */
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	/* LED1 引脚默认输出低电平（不配置 GPIOTE，不请求 HFCLK） */
	nrf_gpio_cfg_output(OUTPUT_PIN);
	nrf_gpio_pin_clear(OUTPUT_PIN);

	/* LED0 快闪 2 次 → 初始化完成 */
	for (int i = 0; i < 2; i++) {
		gpio_pin_set_dt(&led0, 1);
		k_msleep(200);
		gpio_pin_set_dt(&led0, 0);
		k_msleep(200);
	}

	/* CPU 进入永久休眠 */
	k_sleep(K_FOREVER);
	return 0;
}
