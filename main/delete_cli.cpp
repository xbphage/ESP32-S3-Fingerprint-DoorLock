// 串口删除控制台
//
// 交互输出一律用 printf 而不是 ESP_LOGI：终端里看到的就是 command -> result，
// 不带 I (1234) AS608_SLEEP: 这类前缀，用起来像个正常的命令行。
//
// 输入同时监听两路串口，用户插哪根线都能用：
//   · UART0            —— 板载 USB 转串口芯片那条路，也是 IDF console 的输出口
//   · USB Serial/JTAG  —— ESP32-S3 原生 USB 口那条路
// 两路都只是"能读到就算数"，谁先来数据用谁，不做优先级。

#include "delete_cli.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *CLI_TAG = "AS608_CLI";

#define CLI_UART_PORT   UART_NUM_0
#define CLI_UART_BAUD   115200
#define CLI_LINE_MAX    40

// ESP32-S3 上 UART0 的默认脚，也正是 IDF console 用的那一对
#define CLI_UART_TX_GPIO 43
#define CLI_UART_RX_GPIO 44

// 收到字符后静默这么久，就认为这一行发完了。
// 存在的理由：不少串口工具（如 VSCode 的 Serial Monitor）点"发送"时不追加 \r\n，
// 光靠换行断句的话命令永远拼不成一整行。整串命令是连着到的，字符间隔远小于此值，
// 所以不会把一条命令误切成两条。
#define CLI_COMMIT_IDLE_US  (500 * 1000)

static bool    s_inited      = false;
static bool    s_usb_ok      = false;   // USB Serial/JTAG 驱动是否装上
static bool    s_clear_armed = false;   // 已发出"确认清库?"提问，下一行输入当作回答
static bool    s_rx_reported = false;   // 首次收到输入的那条诊断日志是否已打过
static int64_t s_last_rx_us  = 0;
static char    s_line[CLI_LINE_MAX];
static int     s_len = 0;

// ---- 输入端口初始化 --------------------------------------------------

static void cli_init(void)
{
    // UART0 是 IDF console 的口，但默认只往外出、不往里读（除非用了 esp_console）。
    // 所以这里补上接收方向：驱动没装就装一个，波特率跟 console 对齐。
    if (!uart_is_driver_installed(CLI_UART_PORT))
    {
        ESP_ERROR_CHECK(uart_driver_install(CLI_UART_PORT, 1024, 0, 0, NULL, 0));
    }

    uart_config_t cfg = {};
    cfg.baud_rate  = CLI_UART_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(CLI_UART_PORT, &cfg));

    // 引脚显式指到 UART0 的默认脚。console 启动时其实已经连过一遍，这里再设一次是幂等的，
    // 但能避免"依赖驱动安装前恰好留下的映射"这种隐式状态——RX 到底通没通得有个明确说法。
    ESP_ERROR_CHECK(uart_set_pin(CLI_UART_PORT, CLI_UART_TX_GPIO, CLI_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 原生 USB 口那条路。装不上不影响主功能，退化成只有 UART0 可用。
    usb_serial_jtag_driver_config_t usb_cfg = {};
    usb_cfg.tx_buffer_size = 256;
    usb_cfg.rx_buffer_size = 512;
    s_usb_ok = (usb_serial_jtag_driver_install(&usb_cfg) == ESP_OK);

    ESP_LOGI(CLI_TAG, "删除控制台就绪：UART0 %dbps%s，输入 help 看命令",
             CLI_UART_BAUD, s_usb_ok ? " + USB Serial/JTAG" : "");
}

// 非阻塞取一个字节，两路都没有就返回 -1。src 回填数据是从哪一路来的。
static int cli_getc(const char **src)
{
    uint8_t b;
    if (uart_read_bytes(CLI_UART_PORT, &b, 1, 0) == 1)
    {
        *src = "UART0";
        return (int)b;
    }
    if (s_usb_ok && usb_serial_jtag_read_bytes(&b, 1, 0) == 1)
    {
        *src = "USB Serial/JTAG";
        return (int)b;
    }
    return -1;
}

// ---- 命令实现 --------------------------------------------------------

static void cli_help(void)
{
    printf("\r\n");
    printf("  help            显示本帮助\r\n");
    printf("  list            列出已登记的指纹 ID\r\n");
    printf("  del <id>        删除指定 ID 的指纹，例：del 3\r\n");
    printf("  dt              按一下手指，识别出是谁就删谁\r\n");
    printf("  clear           清空整个指纹库（会再问一次）\r\n");
    printf("\r\n");
    printf("  （命令以回车结束；串口工具若不会自动加换行，敲完停顿约半秒也会执行）\r\n");
    printf("\r\n");
}

static void cli_list(IDENTIFIER &zw)
{
    uint8_t index[32];
    if (zw.PS_ReadIndexTable(0, index) != 0x00)
    {
        printf("读索引表失败，模组可能没应答\r\n");
        return;
    }

    int n = 0;
    printf("已登记指纹 ID：");
    for (int id = 0; id < ID_TEMPLATE_MAX; id++)
    {
        // 1FH 的索引表每 1 bit 对应一枚模板，位序沿用 FindFreeID() 的 LSB 在前解释
        if ((index[id >> 3] >> (id & 0x07)) & 0x01)
        {
            printf(" %d", id);
            n++;
        }
    }
    if (n == 0)
        printf("（空）");
    printf("\r\n共 %d 枚\r\n", n);
}

static void cli_del_id(IDENTIFIER &zw, uint16_t id)
{
    printf("删除 ID %u ...\r\n", (unsigned)id);
    if (zw.Del_FR_ID(id))
        printf("已删除 ID %u\r\n", (unsigned)id);
    else
        printf("删除失败\r\n");
}

// 触摸删除：按一下手指，模块自己认人，认出来是谁就把谁的模板删掉。
// 复用 Auto_Verify()，超时用它的默认值（15 秒），不另外给参数。
static void cli_del_touch(IDENTIFIER &zw)
{
    uint16_t id = 0, score = 0;

    printf("请按手指（15 秒内），识别到谁就删谁\r\n");
    if (!zw.Auto_Verify(&id, &score))
    {
        printf("没认出来，取消删除\r\n");
        return;
    }

    printf("识别到 ID %u（得分 %u），删除中...\r\n", (unsigned)id, (unsigned)score);
    if (zw.Del_FR_ID(id))
        printf("已删除 ID %u\r\n", (unsigned)id);
    else
        printf("删除失败\r\n");
}

static void cli_clear_ask(void)
{
    printf("!! 这会删掉全部指纹且无法恢复。确认请输入 yes，其他任意内容取消：\r\n");
    s_clear_armed = true;
}

// 执行一整行命令。line 会被 strtok 就地切开，调用方不用保留。
static void cli_exec(IDENTIFIER &zw, char *line)
{
    // 敲命令也算"有人正在用"，把休眠倒计时推后。
    // 不这么做的话，60 秒不碰指纹设备就断电深睡了，串口控制台跟着一起没了。
    ZW_Sleep(60, zw);

    // 清库确认要赶在 strtok 之前判定：那时 line 还是完整的原始输入
    if (s_clear_armed)
    {
        s_clear_armed = false;
        if (strcmp(line, "yes") == 0)
        {
            zw.Del_FR_Lib();
        }
        else
        {
            printf("已取消，指纹库未改动\r\n");
        }
        return;
    }

    char *cmd = strtok(line, " \t");
    if (cmd == NULL)
        return;                                  // 敲了个空行

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
    {
        cli_help();
    }
    else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
    {
        cli_list(zw);
    }
    else if (strcmp(cmd, "del") == 0 || strcmp(cmd, "d") == 0)
    {
        char *arg = strtok(NULL, " \t");
        if (arg == NULL)
            printf("用法：del <id>，先用 list 看有哪些 ID\r\n");
        else
            cli_del_id(zw, (uint16_t)atoi(arg));
    }
    else if (strcmp(cmd, "dt") == 0 || strcmp(cmd, "deltouch") == 0)
    {
        cli_del_touch(zw);
    }
    else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "clr") == 0)
    {
        cli_clear_ask();
    }
    else
    {
        printf("未知命令：%s（输入 help 看用法）\r\n", cmd);
    }
}

// ---- 对外接口 --------------------------------------------------------

void DeleteCLI_Poll(IDENTIFIER &zw)
{
    if (!s_inited)
    {
        cli_init();
        s_inited = true;
    }

    bool got = false;
    const char *src = "";
    int c;

    while ((c = cli_getc(&src)) >= 0)
    {
        got = true;
        s_last_rx_us = esp_timer_get_time();

        // 只在第一次收到输入时报告，用来一次性确认"数据到底进不进得来、走的哪一路"，
        // 之后就不再刷屏了。
        if (!s_rx_reported)
        {
            s_rx_reported = true;
            ESP_LOGI(CLI_TAG, "首次收到串口输入（来自 %s，首字节 0x%02X），读取通路正常",
                     src, (unsigned)c);
        }

        if (c == '\r' || c == '\n')
        {
            if (s_len > 0)
            {
                s_line[s_len] = '\0';
                s_len = 0;
                cli_exec(zw, s_line);
            }
            continue;
        }

        if (c == 0x08 || c == 0x7f)              // 退格
        {
            if (s_len > 0)
                s_len--;
            continue;
        }

        if (s_len < CLI_LINE_MAX - 1)
            s_line[s_len++] = (char)c;
    }

    // 没收到换行、但已经静默够久，也当作一行结束。见 CLI_COMMIT_IDLE_US 的说明。
    if (!got && s_len > 0 &&
        (esp_timer_get_time() - s_last_rx_us) > CLI_COMMIT_IDLE_US)
    {
        s_line[s_len] = '\0';
        s_len = 0;
        cli_exec(zw, s_line);
    }
}
