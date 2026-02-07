/*
 * 极致低功耗按钮触发 IO 翻转 — 最终版
 *
 * 功能：
 *   按下 BUTTON0 → LED1 每 500ms 翻转一次，共 6 次
 *   CPU 仅在按钮中断时介入一次（启动定时器）
 *   之后完全由硬件（TIMER + GPIOTE + DPPI）自动完成
 *
 * 硬件链路：
 *   TIMER20（500ms 周期定时器）
 *     ─ COMPARE[0] 事件 ─→ DPPI CH 8 ─┬→ GPIOTE20 CH0 翻转 LED1
 *                                       └→ TIMER21 COUNT +1
 *     ─ SHORTS: COMPARE0 → CLEAR（自动重启计时）
 *
 *   TIMER21（计数器，CC[0]=6）
 *     ─ COMPARE[0] 事件 ─→ DPPI CH 9 ─┬→ TIMER20 STOP
 *                                       └→ TIMER21 STOP
 *
 * 电源域：
 *   所有外设在 PERI PD — TIMER20, TIMER21, GPIOTE20, DPPIC20, GPIO1
 *   RADIO PD 已关闭
 *   CPU 在 k_sleep(K_FOREVER) 后进入低功耗休眠
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <hal/nrf_timer.h>
#include <hal/nrf_gpiote.h>
#include <hal/nrf_dppi.h>
#include <hal/nrf_gpio.h>

/* ===== 按钮（sw0 = BUTTON0, P1.13, GPIO1 / PERI PD） ===== */
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* ===== LED0：初始化状态指示（Zephyr GPIO 控制） ===== */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ===== LED1：翻转输出引脚（纯 GPIOTE HAL 控制，不经过 Zephyr GPIO） ===== */
#define OUTPUT_PIN  NRF_GPIO_PIN_MAP(1, 10)  /* P1.10 = LED1 */
#define GPIOTE_CH   0  /* GPIOTE20 通道 0（nrfx 从高编号 CH7 开始分配，CH0 最安全） */

/* ===== DPPI 通道（使用较高编号，避开 Zephyr 内部使用） ===== */
#define DPPI_CH_TOGGLE  8  /* TIMER20 COMPARE → GPIOTE 翻转 + TIMER21 计数 */
#define DPPI_CH_STOP    9  /* TIMER21 COMPARE → TIMER20 停止 + TIMER21 停止 */

/* ================================================================
 * start_toggle_chain()
 * CPU 唯一的操作：启动 TIMER20，之后硬件自动完成 6 次翻转
 * 可重入 — 每次调用都会重置并重新开始
 * ================================================================ */
static void start_toggle_chain(void)
{
	/* 1. 停止可能正在运行的链路 */
	nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(NRF_TIMER21, NRF_TIMER_TASK_STOP);

	/* 2. 清零两个定时器 */
	nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(NRF_TIMER21, NRF_TIMER_TASK_CLEAR);

	/* 3. 清除事件标志（防止残留事件立即触发） */
	nrf_timer_event_clear(NRF_TIMER20, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_event_clear(NRF_TIMER21, NRF_TIMER_EVENT_COMPARE0);

	/* 4. 重新配置 GPIOTE，确保 LED1 从灭状态开始翻转 */
	nrf_gpiote_task_configure(NRF_GPIOTE20, GPIOTE_CH, OUTPUT_PIN,
		NRF_GPIOTE_POLARITY_TOGGLE, NRF_GPIOTE_INITIAL_VALUE_LOW);
	nrf_gpiote_task_enable(NRF_GPIOTE20, GPIOTE_CH);

	/* 5. 启动 TIMER21（计数器必须先 START 才能响应 COUNT 任务） */
	nrf_timer_task_trigger(NRF_TIMER21, NRF_TIMER_TASK_START);

	/* 6. 启动 TIMER20 — 硬件接管，CPU 不再参与 */
	nrf_timer_task_trigger(NRF_TIMER20, NRF_TIMER_TASK_START);
}

/* ================================================================
 * 按钮中断回调（ISR 上下文）
 * 带 300ms 软件消抖
 * ================================================================ */
static volatile int64_t last_press_time;

static void button_pressed(const struct device *dev,
			   struct gpio_callback *cb, uint32_t pins)
{
	int64_t now = k_uptime_get();

	if ((now - last_press_time) < 50) {
		return;  /* 消抖：50ms 内的重复触发忽略 */
	}
	last_press_time = now;

	/* LED0 翻转 = 视觉确认 CPU 响应了按钮 */
	gpio_pin_toggle_dt(&led0);

	/* 启动（或重启）翻转链路 */
	start_toggle_chain();
}

/* ================================================================
 * 初始化硬件链路（上电一次性配置）
 * ================================================================ */
static void init_hardware_chain(void)
{
	/*
	 * --- TIMER20：定时器模式，1 MHz 时钟，500ms 周期 ---
	 * 16 MHz / 2^4 = 1 MHz
	 * CC[0] = 500000 → 500ms
	 * SHORTS: COMPARE0 → CLEAR（自动重启，持续产生周期事件）
	 */
	nrf_timer_mode_set(NRF_TIMER20, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(NRF_TIMER20, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(NRF_TIMER20, NRF_TIMER_FREQ_1MHz);
	nrf_timer_cc_set(NRF_TIMER20, NRF_TIMER_CC_CHANNEL0, 500000);
	nrf_timer_shorts_enable(NRF_TIMER20,
		NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK);

	/*
	 * --- TIMER21：计数器模式，CC[0]=6 ---
	 * 每收到一次 COUNT 任务 → 计数 +1
	 * 计数到 6 时产生 COMPARE[0] 事件 → 用于停止整个链路
	 */
	nrf_timer_mode_set(NRF_TIMER21, NRF_TIMER_MODE_COUNTER);
	nrf_timer_bit_width_set(NRF_TIMER21, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_cc_set(NRF_TIMER21, NRF_TIMER_CC_CHANNEL0, 6);

	/*
	 * --- GPIOTE20 通道 0：P1.10 (LED1) 翻转输出 ---
	 * 每收到一次 OUT[0] 任务 → 引脚电平翻转
	 * 注意：nrfx_gpiote 从 CH7 开始分配（按钮在 CH7），所以 CH0 最安全
	 */
	nrf_gpio_cfg_output(OUTPUT_PIN);
	nrf_gpio_pin_clear(OUTPUT_PIN);
	nrf_gpiote_task_configure(NRF_GPIOTE20, GPIOTE_CH, OUTPUT_PIN,
		NRF_GPIOTE_POLARITY_TOGGLE, NRF_GPIOTE_INITIAL_VALUE_LOW);
	nrf_gpiote_task_enable(NRF_GPIOTE20, GPIOTE_CH);

	/*
	 * --- DPPI 通道 8：TIMER20 COMPARE[0] → 翻转 + 计数 ---
	 * 发布：TIMER20 COMPARE[0] 事件
	 * 订阅：GPIOTE20 OUT[0]（翻转 LED1）
	 * 订阅：TIMER21 COUNT（计数 +1）
	 */
	nrf_timer_publish_set(NRF_TIMER20,
		NRF_TIMER_EVENT_COMPARE0, DPPI_CH_TOGGLE);
	nrf_gpiote_subscribe_set(NRF_GPIOTE20,
		nrf_gpiote_out_task_get(GPIOTE_CH), DPPI_CH_TOGGLE);
	nrf_timer_subscribe_set(NRF_TIMER21,
		NRF_TIMER_TASK_COUNT, DPPI_CH_TOGGLE);

	/*
	 * --- DPPI 通道 9：TIMER21 COMPARE[0] → 停止链路 ---
	 * 发布：TIMER21 COMPARE[0] 事件（计数到 6）
	 * 订阅：TIMER20 STOP（停止定时）
	 * 订阅：TIMER21 STOP（停止计数）
	 */
	nrf_timer_publish_set(NRF_TIMER21,
		NRF_TIMER_EVENT_COMPARE0, DPPI_CH_STOP);
	nrf_timer_subscribe_set(NRF_TIMER20,
		NRF_TIMER_TASK_STOP, DPPI_CH_STOP);
	nrf_timer_subscribe_set(NRF_TIMER21,
		NRF_TIMER_TASK_STOP, DPPI_CH_STOP);

	/*
	 * --- 使能两个 DPPI 通道 ---
	 */
	nrf_dppi_channels_enable(NRF_DPPIC20,
		(1UL << DPPI_CH_TOGGLE) | (1UL << DPPI_CH_STOP));
}

/* ================================================================ */
int main(void)
{
	/* LED0 初始化（Zephyr GPIO，仅用于状态指示） */
	gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);

	/* LED0 亮 1 秒 → 表示程序开始运行 */
	gpio_pin_set_dt(&led0, 1);
	k_msleep(1000);
	gpio_pin_set_dt(&led0, 0);

	/* 先配置按钮中断（让 Zephyr GPIO 驱动先分配 GPIOTE 通道） */
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	/*
	 * 再初始化 DPPI 硬件链路
	 * 必须在按钮配置之后，否则 Zephyr GPIO 驱动分配 GPIOTE 通道时
	 * 会覆盖我们的 GPIOTE20 CH7 配置（导致翻转错误引脚）
	 */
	init_hardware_chain();

	/* LED0 快闪 2 次 → 表示初始化完成，等待按键 */
	for (int i = 0; i < 2; i++) {
		gpio_pin_set_dt(&led0, 1);
		k_msleep(200);
		gpio_pin_set_dt(&led0, 0);
		k_msleep(200);
	}

	/*
	 * CPU 进入永久休眠
	 * 按下 BUTTON0 → 中断唤醒 CPU → 启动硬件链路 → CPU 立即回到休眠
	 * LED1 自动翻转 6 次后停止，全程无需 CPU 参与
	 */
	k_sleep(K_FOREVER);
	return 0;
}
