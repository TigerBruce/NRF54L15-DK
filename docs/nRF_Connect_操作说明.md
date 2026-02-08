# 用 nRF Connect 手机端操作说明

烧录 **main2.c** 固件后，板子会以名称 **「IOTrigger」** 广播，并提供一个可写 GATT 特征，手机写入即触发（IO 由你在代码里处理）。

## 1. 扫描并连接

1. 打开 **nRF Connect**。
2. 在 **Scanner** 里点 **SCAN**，等列表里出现 **IOTrigger**（或你的设备名）。
3. 点 **IOTrigger** 右侧的 **CONNECT**，建立连接。

## 2. 找到自定义 Service 并写入

1. 连接成功后，会进入 **GATT** 界面，展开 **Unknown Service**（或按 UUID 显示的 Service）。
2. 我们的自定义 Service UUID 为 **0x1234**，下面有一个特征 **0x1235**（可写）。
3. 点该特征右侧的 **↓（写）** 图标。
4. 在写入界面：
   - 随便输入 1 个字节（如 `01`）或更多。
   - 点 **Send** / **Write**。
5. 每次写入都会在从机侧触发一次 GATT 写回调（你在 main2.c 或扩展逻辑里处理 IO 即可）。

## 3. 若看不到 Unknown Service

- 在 GATT 列表里找 **UUID 为 1234** 的 Service，其下 **UUID 1235** 即为可写特征。
- 若列表按 128-bit UUID 显示，可在 nRF Connect 里看「Service UUID」是否包含 `1234`（16-bit 短 UUID）。

## 4. 连接参数（固件已请求）

- 固件在连接后会请求：**连接间隔 800 ms**、**Slave Latency 4**，满足 1 秒内响应且低功耗。手机一般会接受，无需在 App 里改。
