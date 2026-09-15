# fingerID —— ZW111 / AS608 指纹模块驱动

组件路径 `components/AS608_ESP-IDF/`，目标芯片 **ESP32-C3**，ESP-IDF **v5.5.5**。

> 本组件原先跑在 ESP32-S3 上，已迁移到 ESP32-C3（不再兼容 S3）。引脚分布和深睡唤醒
> 方式都变了，差异见项目根目录的 [`README.md`](../../README.md) 第 5 节。

协议依据《指纹模组产品用户手册 V1.5.1》，电气与接线依据《ZW111 半导体指纹处理模块规格书 V1.2.2》（两份原文档与转换后的 Markdown 都在仓库 `doc/` 下）。

---

## 1. 概述

`IDENTIFIER` 类封装了模组的 UART 通信、指纹注册/验证、LED 控制、断电休眠四块功能。

当前设计目标是**低功耗门锁式应用**：平时深度休眠、断电养电，手指触摸唤醒后直接做一次指纹验证，验证完重新计时、再次休眠。整条链路由 `main/main.cpp` 的 `app_main()` 串起来。

核心思路是**尽量用模组自带的一体化指令**（`31H` 自动注册、`32H` 自动验证），而不是在主控侧用 `01H/02H/05H/06H` 逐条拼流程。模组内部的 RISC-V 跑这些流程比 MCU 发多条指令往返更快，也更少出错。

---

## 2. 硬件接线

| 模组引脚 | 接到 ESP32-C3 | 说明 |
|---|---|---|
| PIN1 `V_SENSOR` | 3.3V（**常供，不受 MCU 控制**） | 触摸检测靠它供电，断了就唤不醒 |
| PIN2 `TOUCH_OUT` | `GPIO3` | 高电平=活体为真，作 Deep-sleep 唤醒源 |
| PIN3 `VCC` | `GPIO7` → 外部开关电路 → 3.3V | MCU 控制模组通断电 |
| PIN4 `TX` | `GPIO1` | 模组 → MCU |
| PIN5 `RX` | `GPIO0` | MCU → 模组 |
| PIN6 `GND` | GND | |

**UART 参数**：`UART_NUM_1`，**57600** bps，8 数据位 / 1 停止位 / 无校验，3.3V TTL。

> ⚠️ 波特率必须是 57600。模组出厂默认就是它，曾经因为写成 `75600`（数字调换）导致模块一个字节都不回、所有指令都返回确认码 `0xFF`。

### VCC 开关电路（规格书第 5 章）

`GPIO7` 不能直接驱动模组 VCC（工作电流典型 40 mA），要走开关管：

| 位号 | 型号 | 作用 |
|---|---|---|
| Q1 | 9013 (NPN) | 基极经 R1 接 GPIO7，发射极接地 |
| Tr1 | SI2301 (P-MOS) | 源极接 3.3V，漏极接模组 VCC，栅极经 R3 上拉 |
| R1/R2 | 10K | 基极限流 / 下拉 |
| R3 | 100K | 栅极上拉 |

`GPIO7` 拉高 → Q1 导通 → Tr1 栅极被拉低 → Tr1 导通 → 模组上电。

### 引脚占用

| GPIO | 用途 |
|---|---|
| 0 / 1 | 模组 RX / TX（`UART_NUM_ID` = UART1） |
| 3 | 模组 `TOUCH_OUT`（**C3 上只有 GPIO0~GPIO5 能做 Deep-sleep 唤醒**，见下） |
| 6 | PWM（`components/PWM`，舵机） |
| 7 | 模组 VCC 控制 |
| 9 | BOOT 按键（板载，`main/main.cpp` 里读，用于触发注册） |
| 20 / 21 | UART0，串口控制台（板载 USB 转串口） |

### ESP32-C3 的引脚约束

C3 可用的引脚比 S3 少得多，上面这几个位置是被硬件限制逼出来的：

- **GPIO11** 是 `VDD_SPI`，**GPIO12~GPIO17** 被内置 SPI flash 的 CLK/CS/D0~D3 独占 —— 一排 7 个脚直接没了。
- **GPIO18/GPIO19** 是原生 USB 的 D-/D+，所以才把指纹串口挪到 GPIO0/GPIO1。
- **GPIO2 / GPIO8 / GPIO9** 是 strapping 脚：GPIO9 是 BOOT 键，GPIO8 在官方 DevKitM-1 上还焊着 RGB LED，都不适合做舵机 PWM。
- **深睡唤醒只有 GPIO0~GPIO5 合法**（`SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK`），所以 `TOUCH_OUT` 落在 GPIO3。C3 **没有 RTC IO**（`SOC_RTCIO_PIN_COUNT=0`），`SOC_PM_SUPPORT_EXT0_WAKEUP` 也压根没定义，`esp_sleep_enable_ext0_wakeup()` 用不了 —— 深睡唤醒统一走 `esp_deep_sleep_enable_gpio_wakeup()`（见第 6 节）。

---

## 3. 快速上手

```cpp
#include "fingerID.hpp"

// 1) 先给模组上电并等它启动（构造函数之前就必须做）
ID_PowerOn();
vTaskDelay(pdMS_TO_TICKS(ID_POWER_ON_DELAY_MS));

// 2) 构造。内部会装 UART 驱动并做一次握手
IDENTIFIER zw;

// 3) 注册一枚指纹（自动挑 ID、自动录入 5 次）
uint16_t newID;
if (zw.Auto_Enroll(&newID)) {
    ESP_LOGI("app", "注册成功，ID = %u", (unsigned)newID);
}

// 4) 验证（1:N 搜索）
uint16_t id, score;
if (zw.Auto_Verify(&id, &score)) {
    ESP_LOGI("app", "验证通过 ID=%u 得分=%u", (unsigned)id, (unsigned)score);
}
```

> `IDENTIFIER` 的构造函数会调 `init_uart2id()` 和 `AS608_Check()`，后者最长会阻塞约 1.1 秒
> （3 次重试 × 每次 80ms 等待 + 300ms 收包超时），**不要放在中断或定时器回调里构造**。

---

## 4. API 参考

### 4.1 生命周期

| 函数 | 说明 |
|---|---|
| `IDENTIFIER()` | 装 UART 驱动 → 延时 200ms → 握手 `AS608_Check()`，结果存进 `m_online`。UART 驱动用 `uart_is_driver_installed()` 去重，多次构造不会重复注册 |
| `~IDENTIFIER()` | 空实现。UART 驱动是进程级共享资源，不随对象析构释放，避免多实例互相破坏 |
| `bool AS608_Check()` | 连接检查（`13H` 口令验证），最多重试 3 次。返回 `false` 并已打印失败类别；结论同时写进 `m_online` |
| `bool Is_Online()` | 取最近一次连接检查的结论。构造函数里已经握过手，**用它取值即可，不必再调 `AS608_Check()` 重发一遍** |

> ⚠️ **构造函数必须在 `ID_PowerOn()` 之后才执行。** 构造函数内部就会握手，模组没上电时那次握手必然失败，
> 而且会污染 `m_online` —— 之后再调 `Is_Online()` 就分不清"模组真没接"和"对象建早了"。
> 常见写法是 `static IDENTIFIER zw;` 放在 `ID_PowerOn()` + 延时之后（`app_main` 里就是这么做的）。

`AS608_Check()` 的失败原因是分开报的，三类问题的排查方向完全不同：

| 日志 | 含义 | 排查方向 |
|---|---|---|
| `模组无任何应答` | 一个字节都没回来 | 模组是否上电（`ID_VCC_GPIO` 应为高）、TX/RX 是否交叉、波特率是否 57600 |
| `收到了数据，但没有一帧是合法应答包` | 线路上有字节，但切不出合法包 | 优先查 TX/RX 接反、电平不匹配，其次才是波特率 |
| `模组有应答，但确认码是 0x??` | 包合法，模块明确拒绝 | 口令（`IDpwd`）不对，或模块处于异常状态 |

判据特意**不是**"缓冲区里有没有字节"：RX 悬空时线路上会拾到工频杂波，那个判据会把根本没接的模组判成已连接。
所以这里走 `ReadAckPacket()` 做完整的包头 + 长度校验，再按确认码下结论。

### 4.2 自动注册模板 —— `Auto_Enroll()`

```cpp
bool Auto_Enroll(uint16_t *outID,
                 uint8_t   times      = ID_ENROLL_TIMES,      // 默认 5
                 uint32_t  timeout_ms = ID_ENROLL_TIMEOUT_MS  // 默认 90000
                );
```

对应模组指令 `PS_AutoEnroll`（**指令码 `31H`**）。模组自己完成一整条流水线：采图 → 生成特征 → 合并模板 → 重复指纹检查 → 存储模板。

**"自动下一个 ID"** 的实现分两层：

1. `FindFreeID()` 发 `PS_ReadIndexTable`（`1FH`）读第 0 页索引表，32 字节 = 256 bit，每 bit 对应一枚模板，**找第一个为 0 的位**。
2. 索引表读不到、或者猜错了 ID（模块在"合法性检测"阶段返回 `22H` = 该 ID 已有模板），就把 ID 加一继续试，最多试满 `ID_TEMPLATE_MAX` 枚。

> 手册没有写明索引表字节内是 MSB 还是 LSB 在前，代码按 AS608 系惯例取 **LSB 在前**。位序万一猜反也不会出错——模块的 `22H` 会兜住，代价只是多几次往返。

**参数位**选用 `ID_ENROLL_P_LEDOFF | ID_ENROLL_P_NODUP`（`0x0011`）：

| 位 | 值 | 理由 |
|---|---|---|
| bit0 | 1 | 采图成功后灭背光灯，省电 |
| **bit2** | **0** | **必须为 0**（要求返回关键步骤）。置 1 收不到进度包，也无法判断注册何时结束 |
| bit3 | 0 | 不允许覆盖已占用的 ID（安全性） |
| bit4 | 1 | 不允许重复指纹注册，防止同一根手指占两个 ID |
| bit5 | 0 | 要求手指离开才能进入下一次采集，标准按压体验 |

**返回值**：`true` 表示模板已成功存储，`outID` 回填实际使用的 ID。

**进度输出**：函数会把模组的每个关键步骤打印出来（`第 n 次：请按手指` / `抬起手指` / `录入成功` …）。因为参数位 bit2=0，这些进度包一定会收到。

**失败处理**：任何一步出致命错误、或整体超时，都会自动发一次 `PS_Cancel`（`30H`），避免模组卡在半途的注册状态里导致后续指令全部失效。

### 4.3 自动验证指纹 —— `Auto_Verify()`

```cpp
bool Auto_Verify(uint16_t *outID,
                 uint16_t *outScore   = nullptr,
                 uint16_t  id         = ID_VERIFY_ALL_ID,   // 0xFFFF
                 uint8_t   level      = ID_VERIFY_LEVEL,    // 默认 3
                 uint32_t  timeout_ms = ID_VERIFY_TIMEOUT_MS // 默认 15000
                );
```

对应模组指令 `PS_AutoIdentify`（**指令码 `32H`**）。一条指令完成采图 + 生成特征 + 搜索。

| 参数 | 说明 |
|---|---|
| `id` | 填 `ID_VERIFY_ALL_ID`（`0xFFFF`）做 **1:N 搜索**（在整个指纹库里找）；填具体模板号则与该模板做 **1:1 匹配** |
| `level` | 分数等级 `0~9`，越大越严格 |
| `outScore` | 回填匹配得分，可用于自己再做阈值判断 |

**返回值**：`true` 表示验证通过并已回填 `outID` / `outScore`；`false` 表示这一次没通过（未知指纹 / 采图失败 / 指令被拒 / 超时），具体原因会打印出来。

**各确认码的处理**：

| 收到的确认码 | 处理 |
|---|---|
| `00H` | 验证通过，立即返回 `true`，回填 `outID` / `outScore` |
| `09H` | **未知指纹**，打印后返回 `false` |
| `26H` 超时 / `17H` 残留指纹 | 打印后返回 `false` |
| `24H` 库为空 / `0bH` ID 无效 / `23H` 模板不存在 | 打印"指令被拒"后返回 `false` |
| 一直收不到应答直到超时 | 先 `PS_Cancel()` 把模组从那轮的等待状态里拉出来，再返回 `false` |

**不重试**：一次调用只发一条 `32H`，失败就返回。反复重试交给调用方——主循环本来就会下一轮再进来。**也不主动点红灯**：验证失败的红灯提示由模组自己负责（见 4.6）。

> **`09H` 的语义比字面宽**：手册表 3-55 写的是"没搜索到指纹"，但指令说明第 6)、7) 条写明"生成特征失败"和"比对失败"也返回 `09 05H`。所以它实际是"这一次没认出来"的统一码，按"未知指纹"处理即可。

> ⚠️ **调用方注意**：返回 `false` 时函数**不等手指离开**就退出了。如果在主循环里靠"检测到触摸"触发验证，手指还按着的话下一轮会立刻再进来。

### 4.4 模板库管理

| 函数 | 指令 | 说明 |
|---|---|---|
| `void Del_FR(void)` | `0CH` | 删除单个模板，参数**硬编码为 ID=1**，只能用来清掉 1 号 |
| `void Del_FR_Lib(void)` | `0DH` | 清空整个指纹库 |

> ⚠️ `Auto_Enroll` 开了"不允许重复注册"（bit4=1），**同一根手指重复注册会被模组拒绝（确认码 `27H`）**。测试期间要反复注册，得先 `Del_FR_Lib()` 清库，或者把参数位改成允许重复。

### 4.5 低功耗：断电休眠与触摸唤醒

这块由三个部件配合，**进入 Deep-sleep 后芯片是复位重跑的**，`app_main` 会从头再执行一遍。

**① 供电控制（自由函数，不依赖对象）**

```cpp
void ID_PowerOn(void);    // GPIO7 拉高，模组上电
void ID_PowerOff(void);   // GPIO7 拉低，模组断电
```

之所以做成自由函数：唤醒后顺序必须是"先上电 → 等启动 → 再构造 `IDENTIFIER` 去握手"，此时对象还不存在。

**② 倒计时与休眠**

`ZW_Sleep(t, zw)` 装填一个一次性 `esp_timer`，`t` 秒内**没收到模组任何数据**就进休眠。收到数据（`JudgeStr()` / `ReadAckPacket()` 里都会调 `TouchSleepTimer()`）就把倒计时往后推。

- `ZW_Sleep(0, zw)` 表示立刻休眠。
- **倒计时到点后，定时器回调 `PS_Sleep(void *arg)` 直接执行 `EnterDeepSleep()`**。`arg` 就是注册定时器时传进去的 `IDENTIFIER*`，回调自己就能收尾，不需要再往 `app_main` 递一个"请求休眠"的标志来回倒手。
- 代价：回调跑在 `esp_timer` 服务任务上，而 `EnterDeepSleep()` 会一路阻塞几百毫秒（`JudgeStr` 收一次包就要 500ms），期间别的定时器派发不了。反正芯片马上就要复位重跑，可以接受。
- 因为这条链路整个跑在 `esp_timer` 任务的栈上（UART 收发 + `ESP_LOGI` 格式化 + `esp_deep_sleep_start()`），`sdkconfig` 里 **`CONFIG_ESP_TIMER_TASK_STACK_SIZE` 已从默认 3584 提到 5120**。
- 内部用 `esp_timer_stop` + `esp_timer_start_once` 重新装填，**不能用 `esp_timer_restart`**：它要求 `timer_armed()`（`alarm > 0`），而一次性定时器触发过之后 `alarm` 已被清零，`restart` 只会返回 `ESP_ERR_INVALID_STATE` 什么都不做，导致倒计时再也装不上、模块永远不休眠。

> ⚠️ 由此带来一条**对象生命周期**约束：`IDENTIFIER` 实例必须是 `static` 或堆对象。
> `app_main` 末尾不再有 `while(1)` 撑着，它一返回 main task 就被删除、栈被回收，而定时器里存的正是 `&zw`，60 秒后回调再解引用就是**野指针**。见 `main.cpp` 里 `static IDENTIFIER zw;` 处的注释。

**③ 进入休眠 —— `EnterDeepSleep()`**

```cpp
void EnterDeepSleep(void);   // 进入后不再返回
```

顺序：

1. 置 `s_sending_sleep`（防止休眠指令自身的应答把倒计时又推回去），并把倒计时 `esp_timer_stop` 掉。**正常路径下这一步是从该定时器自己的回调里执行的**——一次性定时器在触发瞬间就已出队、`alarm` 归零，所以 `stop` 返回 `ESP_ERR_INVALID_STATE`，属正常，返回值故意不检查
2. `PS_LedOff()` —— `3CH` + 功能码 `04H` 常闭灯
3. `PS_Sleep()` —— `33H`，模组自身进休眠（静态约 10µA）
4. `ID_PowerOff()` → `gpio_hold_en(IO7)` → `gpio_deep_sleep_hold_en()`
5. 把 MCU 的 TX 改成高阻并 hold，避免模组断电后电流从 TX 倒灌进它的 VCC 轨
6. `esp_deep_sleep_enable_gpio_wakeup(1ULL << ID_TOUCH_OUT_GPIO, ESP_GPIO_WAKEUP_GPIO_HIGH)` —— 高电平唤醒。
   C3 没有 RTC IO，也没有 `EXT0`，只能用这个 API；掩码里只有 GPIO0~GPIO5 合法，越界会返回 `ESP_ERR_INVALID_ARG`。
   它自己不碰引脚配置，上下拉是 `esp_deep_sleep_start()` 内部按唤醒电平装的（高电平唤醒 ⇒ 内部下拉），所以不触摸时该脚不会被拉高、不会误唤醒
7. `esp_deep_sleep_start()`

> `gpio_deep_sleep_hold_en()` 不能省：Deep-sleep 时数字域掉电，不 hold 的话 IO7 会浮空，模组可能被重新上电。

**④ 唤醒后的恢复**（在 `app_main` 里）

```
esp_sleep_get_wakeup_cause() 判定
  ├─ 冷启动         → 正常初始化 → ZW_Sleep(60) → app_main 返回
  └─ GPIO 触摸唤醒  → 解除 gpio hold → ID_PowerOn() → 等 300ms
                    → 构造 IDENTIFIER → PS_LedAuto() 恢复 LED
                    → Auto_Verify() 验证 → 回到 ZW_Sleep(60) → app_main 返回
```

两条分支最后都**不再有主循环兜底**——`ZW_Sleep(60)` 装好倒计时，`app_main` 就地返回，之后的事全交给定时器回调。

**唤醒后必须解除 gpio hold**（`gpio_deep_sleep_hold_dis()` + `gpio_hold_dis()`），否则 IO7 和 TX 会被锁死，模组永远上不了电、串口也发不出数据。

**⑤ 轮询触摸 —— `Is_Touch()`**

```cpp
bool Is_Touch(void);   // 轮询 TOUCH_OUT，当前有没有手指按在传感器上（高电平 = 活体检测为真）
```

冷启动路径不经过 Deep-sleep，`app_main` 靠主循环轮询它来触发验证。

> ⚠️ **这个脚必须显式配成输入，否则读回来永远是 0。**
>
> 全项目只有 `EnterDeepSleep()` 里的 `esp_deep_sleep_enable_gpio_wakeup()` 碰过 `TOUCH_OUT`，而那条路径只有"准备休眠"时才会走到（何况这个 API 本身并不配置引脚，只登记唤醒源）。冷启动时它还是上电复位状态，而 ESP-IDF 明确规定——
> `esp_driver_gpio/include/driver/gpio.h:146`：
> *"If the pad is not configured for input (or input and output) the returned value is always 0."*
>
> **没配成输入的 pad，`gpio_get_level()` 恒返回 0。** 结果就是首次上电后主循环永远看不到触摸、按指纹没反应；而触摸唤醒那条路走 `from_touch` 分支直接调 `Auto_Verify()`、根本不读这个脚，所以唤醒后反而是正常的——"只有第一次上电不灵"这个现象就是这么来的。
>
> `Is_Touch()` 内部做了一次性 `gpio_config()`（输入 + 下拉）来修掉它。

### 4.6 LED 控制

| 函数 | 指令 | 说明 |
|---|---|---|
| `uint8_t PS_LedCtrl(uint8_t func)` | `3CH` | 通用呼吸灯控制。功能码：`01`呼吸 / `02`闪烁 / `03`常开 / `04`常闭 / `05`渐开 / `06`渐灭 |
| `uint8_t PS_LedOff(void)` | `3CH`+`04H` | 休眠前关灯 |
| `uint8_t PS_LedAuto(void)` | `60H`+`FFH` | 恢复**出厂默认**的自动呼吸灯效果 |

**验证失败的红灯由模组自己负责，驱动不干预。** 模组处于出厂自动模式时（`60H`+`FFH`），手册 3.5.6 写明它的行为是："上电后蓝灯呼吸，注册、验证通过，绿灯常亮，**注册、验证失败红灯闪三次**"。主控一旦发 `3CH` 就切到手动模式，反而要把灯的控制权抢过来自己维护，得不偿失——所以 `Auto_Verify()` 失败时不发任何灯的指令。

**颜色码**（手册表 3-101，`bit0`=蓝 `bit1`=绿 `bit2`=红）：

| 码 | 0x00 | 0x01 | 0x02 | 0x04 | 0x06 | 0x05 | 0x03 | 0x07 |
|---|---|---|---|---|---|---|---|---|
| 颜色 | 全灭 | 蓝 | 绿 | 红 | 红绿 | 红蓝 | 绿蓝 | 红绿蓝 |

头文件里对应 `ID_LED_RED` / `ID_LED_GREEN` / `ID_LED_BLUE` / `ID_LED_OFF`，功能码对应 `ID_LED_FUNC_BLINK` / `ID_LED_FUNC_ON` 等——目前没有调用点，留着备用。

> 恢复 LED 用的是 `PS_BlnAmSw`（`60H`）而不是 `PS_ControlBLN` 的"常开灯"，因为 `60H`+`FFH` 才是模组真正的出厂默认状态。模组掉过电，恢复默认最稳妥。

> 底层 `PS_LedCtrlEx(func, startColor, endColor, loop)` 才是完整版本（能指定颜色和循环次数）；`PS_LedCtrl()` 是它把颜色写死成全灭的简化版（为兼容原有调用保留）。

### 4.7 底层指令（一般不用）

| 函数 | 指令 | 说明 |
|---|---|---|
| `PS_HandShake(uint32_t*)` | `13H` | 握手，返回模块地址 |
| `PS_Sleep()` | `33H` | 模组自身休眠 |
| `PS_Cancel()` | `30H` | 中止进行中的自动注册/自动验证 |
| `PS_ReadIndexTable(page, buf)` | `1FH` | 读模板索引表，32 字节出参 |
| `FindFreeID(start, end)` | — | 组合封装，返回下一个空闲 ID，`0xFFFF` = 库满 |

`PS_Sleep()` 有**两个同名函数**，别搞混：

- `IDENTIFIER::PS_Sleep()` —— 成员函数，发 `33H` 指令
- `PS_Sleep(void *arg)` —— 自由函数，是 `esp_timer` 的**回调**，只置休眠请求标志

---

## 5. 通信协议细节

### 包格式

**命令包**（MCU → 模组）：

```
包头(2) | 地址(4) | 包标识(1)=01H | 包长度(2) | 指令码(1) | 参数(n) | 校验和(2)
```

**应答包**（模组 → MCU）：

```
包头(2) | 地址(4) | 包标识(1)=07H | 包长度(2) | 确认码(1) | 参数(n) | 校验和(2)
```

- `包长度` 计的是**从确认码到校验和**的字节数，所以 **参数区长度 = 包长度 − 3**。
- **校验和 = 包标识 + 包长度高字节 + 包长度低字节 + 指令码 + 所有参数**，取低 16 位。

本次用到的几条：

| 用途 | 指令码 | 包长度 | 参数 | 校验和 |
|---|---|---|---|---|
| 连接检查 | `13H` | `0007H` | 口令(4) | — |
| 读索引表 | `1FH` | `0004H` | 页码(1) | `01+04+1F+page` |
| 取消 | `30H` | `0003H` | — | `0x0034` |
| 自动注册 | `31H` | `0008H` | ID号(2) 次数(1) 参数(2) | `01+08+31+ID+次+参` |
| 自动验证 | `32H` | `0008H` | 等级(1) ID号(2) 参数(2) | `01+08+32+等级+ID+参` |
| 模组休眠 | `33H` | `0003H` | — | `0x0037` |
| LED 控制 | `3CH` | `0007H` | 功能码 起始色 结束色 循环次数 | `01+07+3C+功能码` |
| LED 模式 | `60H` | `0004H` | 功能码(1) | `0x0164`（功能码 FFH） |

### 两种收包方式

| | `JudgeStr()` | `ReadAckPacket()` |
|---|---|---|
| 原理 | 干等 500ms，再扫缓冲区里有没有 `EF 01 + 地址 + 07` 包头 | 按 `包长度` 字段精确切包 |
| 适用 | 一问一答的单包指令（握手、LED、休眠…） | **连续多包**的流水线（自动注册） |
| 不足 | 只抓得到第一个包 | — |

`ReadAckPacket()` 同步包头时遇到不匹配的字节会**丢弃重找**，所以即使某次读歪了也能自动纠正，不会一路错到底。

> 自动注册必须用 `ReadAckPacket()`：它会连续吐出十几个进度包（每一轮的采图/生成特征/手指离开各一个，最后还有合并、查重、存储），用 `JudgeStr()` 只能抓到第一个，决定成败的"模板存储结果"就丢了。

### 自动注册的进度包（参数1 = 步骤，参数2 = 明细）

| 参数1 | 步骤 | 参数2 |
|---|---|---|
| `00H` | 指纹合法性检测 | `00H` |
| `01H` | 获取图像 | `n` = 第 n 次 |
| `02H` | 生成特征 | `n` |
| `03H` | 判断手指离开 | `n` |
| `04H` | 合并模板 | `F0H` |
| `05H` | 注册检验（查重） | `F1H` |
| `06H` | 存储模板 | `F2H` |

### 确认码

**自动注册（手册表 3-52，指令 `31H`）**

| 确认码 | 含义 |
|---|---|
| `00H` | 成功 |
| `01H` | 失败 |
| `07H` | 生成特征失败 |
| `0AH` | 合并模板失败 |
| `0BH` | ID 号超出范围 |
| `1FH` | 指纹库已满 |
| `22H` | 指纹模板非空 |
| `25H` | 录入次数设置错误 |
| `26H` | 超时 |
| `27H` | 指纹已存在 |

**自动验证（手册表 3-55，指令 `32H`）**

| 确认码 | 含义 |
|---|---|
| `00H` | 成功 |
| `01H` | 失败 |
| `07H` | 生成特征失败 |
| `09H` | 没搜索到指纹（未知指纹） |
| `0BH` | ID 号超出范围 |
| `17H` | 残留指纹 |
| `23H` | 指纹模板为空 |
| `24H` | 指纹库为空 |
| `26H` | 超时 |
| `27H` | 指纹已存在 |

> `FFH` **不是模组的确认码**，是驱动自己填的"没收到任何应答"。

两个表的代码实现分别是 `EnrollAckMsg()` 和 `VerifyAckMsg()`（`fingerID.cpp`）。**注册和验证不能共用一张表**——同名确认码在两边含义不同，比如 `22H` 在注册里是"模板非空"、`27H` 在注册里是"指纹已存在"，而验证里压根不会出现这两个码。

---

## 6. 配置宏一览

| 宏 | 默认值 | 说明 |
|---|---|---|
| `ID_VCC_GPIO` | `GPIO_NUM_7` | 模组 VCC 控制脚 |
| `UART_NUM_ID` / `_TX` / `_RX` | `UART_NUM_1` / `GPIO_NUM_0` / `GPIO_NUM_1` | 指纹模组串口（57600 8N1） |
| `ID_TOUCH_OUT_GPIO` | `GPIO_NUM_3` | 触摸唤醒脚，**C3 上必须在 GPIO0~GPIO5 之间** |
| `ID_POWER_ON_DELAY_MS` | `300` | 上电后等模组启动 |
| `ID_ENROLL_TIMES` | `5` | 自动注册录入次数（手册规定 2~10） |
| `ID_VERIFY_LEVEL` | `3` | 自动验证分数等级 0~9 |
| `ID_TEMPLATE_MAX` | `100` | 指纹库容量（规格书标称 100 枚） |
| `ID_VERIFY_ALL_ID` | `0xFFFF` | 自动验证 1:N 搜索的魔数 |
| `ID_ENROLL_TIMEOUT_MS` | `90000` | 自动注册整体超时 |
| `ID_VERIFY_TIMEOUT_MS` | `15000` | 自动验证超时 |
| `ID_LED_RED` / `_GREEN` / `_BLUE` / `_OFF` | `0x04` / `0x02` / `0x01` / `0x00` | 呼吸灯颜色码（表 3-101），当前无调用点 |
| `ID_LED_FUNC_BREATH` / `_BLINK` / `_ON` / `_OFF` | `0x01` / `0x02` / `0x03` / `0x04` | 呼吸灯功能码，当前无调用点 |
| `UART_NUM_ID` / `_TX` / `_RX` | `UART_NUM_1` / `17` / `18` | 串口与引脚 |
| `ID_IDENTIFY_TIMEOUT_MS` | `10000` | **当前无人使用**，原 `press_FR()` 的超时参数 |

表外还有一个改动，不在代码里而在 `sdkconfig`：**`CONFIG_ESP_TIMER_TASK_STACK_SIZE` 从默认 `3584` 提到了 `5120`**。因为休眠回调跑在 `esp_timer` 任务上，整条 `EnterDeepSleep()` 链路用的都是它那份栈（见 4.5 节②）。

---

## 7. 应用层如何配合

`main/main.cpp` 的 `app_main()` 是唯一的串接点：

```
app_main
 ├ 读 esp_sleep_get_wakeup_cause()
 ├ 解除 gpio hold（IO7 / TX）
 ├ ID_PowerOn() + 等 300ms
 ├ pwm_init()
 ├ 构造 static IDENTIFIER（装串口 + 握手）   ← 必须 static/堆，见 4.5 节②
 ├ if (触摸唤醒) { PS_LedAuto(); Auto_Verify(); }
 └ ZW_Sleep(60, zw)      ← 装好倒计时，app_main 就地返回

（60 秒内无模组数据）
   └→ esp_timer 回调 PS_Sleep((void*)&zw)
        └→ zw->EnterDeepSleep()   // 关灯 → 模组休眠 → 断 VCC → 配唤醒源，不返回
```

**整条循环**：60 秒无数据 → 关灯 → 模组休眠 → 断 VCC → Deep-sleep → 手指触摸 → 复位重跑 → 上电 → 验证 → 再计时 60 秒 → 再休眠……

`app_main` 从"装好倒计时"之后就撒手不管了，**没有主循环**——这是刻意的：省掉一个 500ms 轮询循环，也让休眠动作和它的触发条件待在同一个地方。

---

## 8. 故障排查

| 现象 | 排查方向 |
|---|---|
| 所有指令都返回 `0xFF` | 模组根本没应答。查三点：**波特率是否 57600**、`GPIO7` 是否已拉高供电、TX/RX 是否交叉接对 |
| 握手失败 | 看 `AS608_LINK` 那条日志落在上表哪一类，三类原因不一样；另外确认构造 `IDENTIFIER` 之前调过 `ID_PowerOn()` 并等够了启动时间 |
| 注册返回 `22H` | 该 ID 已有模板。正常情况驱动会自动换下一个 ID；若持续出现，说明索引表位序反了 |
| 注册返回 `27H` | 这枚指纹已注册过。`Auto_Enroll` 参数位 bit4=1 不允许重复注册，先 `Del_FR_Lib()` 清库 |
| 注册返回 `1FH` | 指纹库满，删掉不用的模板 |
| 注册返回 `25H` | 录入次数不在 2~10 范围内 |
| **首次上电按指纹没反应，但休眠唤醒后正常** | `TOUCH_OUT`(GPIO3) 没配成输入，`gpio_get_level()` 恒返回 0。见 4.5 节⑤ |
| 一直不休眠 | 检查是否**只有发指令时**才重置倒计时——必须由**收到模组数据**触发；另外确认没有别的任务在持续读串口 |
| 没到 60 秒就重启 | 多半是 `esp_timer` 任务**栈溢出**（休眠回调整条链路跑在它那份栈上）。看 panic 打印里的任务名是不是 `esp_timer`，是就把 `CONFIG_ESP_TIMER_TASK_STACK_SIZE` 再调大 |
| 休眠倒计时到点后卡死/重启 | 检查 `IDENTIFIER` 对象是不是 `static` 或堆对象——普通局部变量会随 `app_main` 返回被回收，定时器里的 `&zw` 变野指针 |
| 唤醒后模组不上电 | 忘了 `gpio_deep_sleep_hold_dis()` / `gpio_hold_dis()` |
| 唤醒后串口发不出数据 | 同上，TX 也被 hold 住了 |

串口输出里 `[AS608_SLEEP]` 和 `[AS608_AUTO]` 两个 TAG 是驱动的日志。

---

## 9. 被取代并已注释掉的内容

以下函数已被一体化指令取代，**实现整段加了 `//` 注释**保留在文件里。需要恢复时把对应段落每行行首的 `// ` 删掉即可（`fingerID.cpp` 里有明显的分隔标注）。

| 已注释 | 被谁取代 |
|---|---|
| `Add_FR()` | `Auto_Enroll()` —— 手动按两次手指 → 自动挑 ID、录入 5 次 |
| `press_FR()` | `Auto_Verify()` —— 发三条指令自己比对 → 一条 `32H` 搞定 |
| `PS_GetImage()` | `Auto_Enroll(31H)` 内部完成，见 `ReadAckPacket()` |
| `PS_GenChar()` | 同上 |
| `PS_Match()` | `Auto_Verify(32H)` 传具体 ID 即 1:1 匹配 |
| `PS_Search()` | `Auto_Verify(32H)` 传 `0xFFFF` 即 1:N 搜索（**改动前就已无调用者**） |
| `PS_RegModel()` | `Auto_Enroll(31H)` 内部完成 |
| `PS_StoreChar()` | 同上 |
| `PS_HighSpeedSearch()` | `Auto_Verify(32H)` |

**保留未注释的**：`PS_DeletChar` / `PS_Empty`（模板删除）、`PS_ReadSysPara`、`PS_SetAddr`、`PS_ReadNotepad` / `PS_WriteNotepad`、`PS_ValidTempleteNum`、`PS_GetRandomCode`、`EnsureMessage` / `ShowErrMessage`。

> 注：`PS_WriteNotepad`、`PS_ValidTempleteNum` 目前**没有任何调用者**，是没被取代、但当下用不到的功能，按原样保留。

### 随之变成未使用的定义

| 名字 | 位置 | 说明 |
|---|---|---|
| `SearchResult` | `fingerID.hpp` 私有 typedef | 原供 `PS_Search` / `PS_HighSpeedSearch` / `press_FR` 用，均已注释 |
| `CharBuffer1` / `CharBuffer2` | `fingerID.hpp` 宏 | 原供 `PS_GenChar` / `Add_FR` / `press_FR` 用，均已注释 |
| `ID_IDENTIFY_TIMEOUT_MS` | `fingerID.hpp` 宏 | 原 `press_FR()` 的超时默认值 |

这几个都是 `#define` 和类型别名，留着不占空间也不产生告警，暂不清除。

---

## 10. 验证状态

- **编译验证**：`tools/idf.sh build` 通过，零 warning 零 error（ESP-IDF v5.5.5 / xtensa-esp-elf GCC 14.2.0）。
- **硬件验证**：`Auto_Enroll` / `Auto_Verify` / `PS_ReadIndexTable` 这一套**尚未在实机上跑过**。断电休眠、触摸唤醒、LED 恢复同样未上硬件。
