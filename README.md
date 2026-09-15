# 指纹门锁固件（ESP32-C3 + ZW111 / AS608）

用 ZW111 指纹模组做识别，舵机做门锁执行，平时断电深睡、靠摸一下模组唤醒。

- 主控：**ESP32-C3**（单核 RISC-V，4MB flash，官方 DevKitM-1 类板子）
- 模组：ZW111（AS608 兼容），UART 57600 8N1
- 框架：ESP-IDF v5.5.5

> 本项目已从 ESP32-S3 迁移到 ESP32-C3，**不再兼容 S3**。引脚分布和唤醒方式都变了，
> 详见 [第 5 节](#5-从-esp32-s3-迁移到-esp32-c3)。S3 时期的配置文件保留在
> `sdkconfig.esp32s3.bak`，确认不需要后可以删掉。

---

## 1. 引脚对照表

### 1.1 本项目占用的引脚

| ESP32-C3 | 信号 | 方向 | 接到哪 |
|---|---|---|---|
| **GPIO0** | 指纹串口 TX | 输出 | 模组 `RX`（PIN5） |
| **GPIO1** | 指纹串口 RX | 输入 | 模组 `TX`（PIN4） |
| **GPIO3** | `TOUCH_OUT` | 输入 | 模组 `TOUCH_OUT`（PIN2） |
| **GPIO6** | 舵机 PWM | 输出 | 舵机信号线（50Hz） |
| **GPIO7** | 模组 VCC 开关 | 输出 | 开关电路 Q1 基极 |
| **GPIO9** | BOOT 按键 | 输入 | 板载 BOOT 键，低电平有效 |
| **GPIO20 / GPIO21** | 串口控制台 RX / TX | 输入 / 输出 | 板载 USB 转串口芯片 |

模组供电：

| 模组引脚 | 接到哪 | 说明 |
|---|---|---|
| PIN1 `V_SENSOR` | 3.3V（**常供，不受 MCU 控制**） | 触摸检测靠它供电，断了就唤不醒 |
| PIN3 `VCC` | 开关电路 → 3.3V | 由 GPIO7 控制通断电 |
| PIN6 `GND` | GND | |

**接线上最容易错的两处**：① MCU 的 TX 要接模组的 RX，交叉接；② `V_SENSOR` 必须常供电，
不能跟 `VCC` 一起被开关电路切断，否则休眠后摸不醒。

### 1.2 与 ESP32-S3 版本的引脚变化

| 信号 | 原来（S3） | 现在（C3） | 为什么必须改 |
|---|---|---|---|
| 指纹串口 TX | GPIO17 | **GPIO0** | C3 上 GPIO12~17 被内置 flash 独占 |
| 指纹串口 RX | GPIO18 | **GPIO1** | C3 上 GPIO18 是原生 USB 的 D- |
| `TOUCH_OUT` | GPIO6 | **GPIO3** | C3 只有 GPIO0~GPIO5 能做深睡唤醒 |
| 舵机 PWM | GPIO8 | **GPIO6** | C3 上 GPIO8 是 strapping 脚，DevKitM-1 还在这上面焊了 RGB LED |
| BOOT 按键 | GPIO0 | **GPIO9** | C3 的 BOOT 键在 GPIO9 |
| 串口控制台 | GPIO43 / 44 | **GPIO21 / 20** | C3 的 UART0 默认脚 |
| 模组 VCC 控制 | GPIO7 | GPIO7 | 不变 |

### 1.3 这些引脚为什么不能用

| 引脚 | 原因 |
|---|---|
| GPIO11 | `VDD_SPI`，模组内部用 |
| GPIO12 ~ GPIO17 | 内置 SPI flash 的 CLK / CS / D0~D3，独占 |
| GPIO18 / GPIO19 | 原生 USB 的 D- / D+，板上有 USB 座 |
| GPIO20 / GPIO21 | UART0，已给串口控制台 |
| GPIO8 | 板载 RGB LED（且是 strapping 脚） |
| GPIO2 | strapping 脚 |
| GPIO9 | 已给 BOOT 键 |

---

## 2. 构建与烧录

构建必须走 `tools/idf.sh` 包装脚本，**不要直接调 `idf.py`**：

```bash
tools/idf.sh build
tools/idf.sh -p COM3 flash monitor
```

原因（详见注释在脚本里）：① 用户 profile 会注入 `MSYSTEM=MINGW64`，IDF 5.5 的
`tools/idf.py` 一见到它就只打印一句提示然后什么也不做、还返回 0；② 脚本里显式指定了
venv 的 python 和两套工具链路径（C3 用 riscv32-esp-elf，脚本里也挂了
xtensa-esp-elf 以便回退到 S3）。

项目级配置写在 `sdkconfig.defaults` 里（4MB flash、esp_timer 任务栈 5120）。
`sdkconfig` 是生成物，删掉后重新构建会从 defaults 重新生成。

---

## 3. 运行流程

```
上电 / 深睡唤醒（芯片复位重跑 app_main）
  │
  ├─ 解除深睡时对 IO7、TX 的 gpio_hold
  ├─ 给模组上电，等 300ms 启动
  ├─ pwm_init()
  ├─ 构造 IDENTIFIER → 握手（13H，UART1 @57600，最多重试 3 次）
  │     └─ 结果用 Is_Online() 取，失败原因分「无应答 / 乱码 / 确认码不对」三类打印
  ├─ 启动 60 秒无数据休眠倒计时
  └─ 主循环
       ├─ 串口删除控制台轮询（非阻塞）
       ├─ 有手指触摸 → Auto_Verify() 识别 → 验证通过则 open_door()
       ├─ BOOT 按键按下 → Auto_Enroll() 注册新指纹
       └─ vTaskDelay(50ms)
```

- **开门**：舵机转到 180° 保持 5 秒，再回 0°。这 5 秒内不响应任何触摸。
- **注册**：按 BOOT 键触发，自动挑一个空闲 ID，连续按 5 次手指合成模板。
- **休眠**：60 秒内没有和模组通信就进 Deep-sleep —— 关灯、模组自身休眠、切断模组 VCC、
  配置 `TOUCH_OUT` 为唤醒源。手指触摸时模组拉高 `TOUCH_OUT`，把 MCU 唤醒。

> 主循环里那句 `vTaskDelay(pdMS_TO_TICKS(50))` 不能删。main 任务优先级(1)高于 IDLE(0)，
> 循环里一旦没有阻塞点就会把 IDLE 饿死，Task WDT 会在 5 秒后报
> `IDLE0 did not reset the watchdog in time`。

---

## 4. 串口控制台

设备插上电脑就能删指纹，不用为了删一条记录去改代码重烧。
在任意一路串口（UART0 或 USB Serial/JTAG）敲命令即可：

| 命令 | 作用 |
|---|---|
| `help` | 显示帮助 |
| `list` | 列出已登记的指纹 ID |
| `del <id>` | 删除指定 ID，例：`del 3` |
| `dt` | 按一下手指，识别出是谁就删谁 |
| `clear` | 清空整个指纹库（会再问一次，输入 `yes` 确认） |

命令以回车结束；串口工具如果不自动加换行，敲完停顿约半秒也会执行。
每收到一条命令会把休眠倒计时推后 60 秒，免得敲着敲着设备睡了。

实现见 `main/delete_cli.cpp`。它挂在主循环里轮询而不是单开一个任务——指纹模组只有一路
UART，开任务就得引入互斥锁、还要处理「主循环正卡在 `Auto_Verify()` 里最长 15 秒」的
竞争，顺序执行天然没有并发问题。

---

## 5. 从 ESP32-S3 迁移到 ESP32-C3

| 差异 | 说明 |
|---|---|
| **单核** | `SOC_CPU_CORES_NUM=1`，`CONFIG_FREERTOS_UNICORE` 自动开启，CPU1 相关的 WDT 配置项消失 |
| **没有 RTC IO** | `SOC_RTCIO_PIN_COUNT=0`，`SOC_PM_SUPPORT_EXT0_WAKEUP` 未定义 —— `esp_sleep_enable_ext0_wakeup()` 在 C3 上不可用 |
| **深睡唤醒改用 GPIO 唤醒** | `esp_deep_sleep_enable_gpio_wakeup(掩码, 电平)`，且掩码里只有 **GPIO0~GPIO5** 合法（`SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK`） |
| **唤醒原因变了** | `esp_sleep_get_wakeup_cause()` 返回 `ESP_SLEEP_WAKEUP_GPIO`，不再是 `EXT0` |
| **引脚少且被占用多** | GPIO12~17 归 flash、18/19 归 USB，实际可自由分配的只有 GPIO0~10（去掉 strapping 和 BOOT 键） |
| **工具链** | riscv32-esp-elf（S3 是 xtensa-esp-elf） |

`gpio_hold_en()` / `gpio_deep_sleep_hold_en()` 在 C3 上照常可用
（C3 没有 `SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP`，走的正是全局 hold 这条分支），
所以休眠前"钉住 VCC 和 TX"的那套逻辑原样保留。

---

## 6. 目录结构

```
main/
  main.cpp          主流程：上电、主循环、开门、注册
  delete_cli.cpp    串口删除控制台
  delete_cli.hpp
components/
  AS608_ESP-IDF/    指纹模组驱动（协议、自动注册/验证、低功耗）
  PWM/              舵机 PWM（LEDC，50Hz）
  MOTOR/            开锁动作（转 180° → 等 5 秒 → 回 0°）
  EXTI/             早期外部中断残留，当前无人调用，链接时会被丢弃
tools/
  idf.sh            构建包装脚本（必用）
doc/                模组规格书
```

驱动层的详细文档（协议格式、确认码表、API 参考）在
[`components/AS608_ESP-IDF/README.md`](components/AS608_ESP-IDF/README.md)。

---

## 7. 已知限制

- **尚未在 C3 硬件上实测过**。代码已按 C3 的约束改完并构建通过，但引脚、深睡唤醒、
  舵机动作都还没上板验证。
- BOOT 按键没有边沿检测和防抖，读到低电平就调 `Auto_Enroll()`（阻塞最长 90 秒）。
  按键接触不良或引脚被拉低会反复触发注册流程。
- `Auto_Verify()` 最长阻塞 15 秒、`open_door()` 阻塞 5 秒，这两段窗口内不响应触摸。
- 主循环在 `Auto_Verify()` 返回后不会等手指抬起，按住不放会反复触发识别。
- `Auto_Enroll()` 目前唯一的触发入口是 BOOT 按键。
