#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwm.h"
#include "fingerID.hpp"
#include "delete_cli.hpp"
#include "esp_log.h"
#include "motor.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

// ESP32-C3 的 BOOT 键是 GPIO9（S3 上才是 GPIO0），低电平有效。
// 这是 C3 的 strapping 脚之一，上电瞬间被采样决定启动模式，按下即进下载模式，
// 但作为普通按键输入使用没有问题。
#define BOOT_BUTTON_GPIO GPIO_NUM_9

#ifdef __cplusplus
extern "C" {
#endif

extern "C" void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // 使能内部上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    // Deep-sleep 唤醒后芯片是复位重跑的，app_main 会从头再执行一遍，
    // 所以这里必须先看唤醒原因，决定走"冷启动"还是"触摸唤醒"。
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    // ESP32-C3 不支持 EXT0/EXT1 唤醒，深睡的 GPIO 唤醒统一报 ESP_SLEEP_WAKEUP_GPIO。
    // 本项目里 GPIO 唤醒源只有 TOUCH_OUT 一个，所以它等价于"被手指摸醒的"。
    bool from_touch = (cause == ESP_SLEEP_WAKEUP_GPIO);

    if (from_touch){
        ESP_LOGI("main","唤醒原因：TOUCH_OUT 触摸中断");
    } else {
        ESP_LOGI("main","唤醒原因：冷启动(%d)", (int)cause);
    }

    // 休眠前用 gpio_hold 把 IO7 和 TX 钉住了，唤醒后必须先解除保持，
    // 否则这两只脚会被锁死，模组永远上不了电、串口也发不出数据。
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(ID_VCC_GPIO);
    gpio_hold_dis(UART_NUM_ID_TX);

    // 不管冷启动还是触摸唤醒，都得先给模组上电并等它启动完，
    // 之后构造 IDENTIFIER 时的 init_uart2id() + AS608_Check() 才谈得上。
    ID_PowerOn();
    vTaskDelay(pdMS_TO_TICKS(ID_POWER_ON_DELAY_MS));

    pwm_init();

    // 必须是 static（或堆）对象，不能是普通局部变量：
    // app_main 末尾不再有 while(1) 撑着，一旦它返回，main task 会被删除、它的栈失效，
    // 而休眠定时器里存的正是 &zw，60 秒后回调再解引用就是野指针。
    // static 把它放进 .bss，一直活到进入 Deep-sleep 芯片复位为止。
    //
    // 声明必须放在 ID_PowerOn() 之后：构造函数内部就会握一次手
    // （init_uart2id() + AS608_Check()），模组还没上电的话这次握手必然失败，
    // 而且结论会被记进 m_online —— 那样就分不清"模组真没接"和"对象建早了"了。
    static IDENTIFIER zw;

    // 上面构造时已经握过手了，这里只取结论，不再重发指令。
    // 失败时 AS608_Check() 已经把排查方向（无应答 / 乱码 / 确认码不对）打出来了。
    if (zw.Is_Online()){
        ESP_LOGI("main","指纹模块握手成功");
    } else {
        ESP_LOGE("main","指纹模块握手失败，功能不可用（检查供电/接线/波特率 57600）");
    }
    ESP_LOGI("main","初始化完成");

    ZW_Sleep(7, zw);

    zw.PS_LedAuto();
    uint16_t matchID = 0, score = 0;
    while(1){
        // 串口删除控制台（help / list / del <id> / dt / clear）。
        // 非阻塞：没敲命令时读一下就返回，不影响这里的轮询节奏。
        DeleteCLI_Poll(zw);

        if (zw.Is_Touch()){
            
            if (zw.Auto_Verify(&matchID, &score)){
                ESP_LOGI("main","指纹识别成功，ID=%u 得分=%u",
                            (unsigned)matchID, (unsigned)score);
                open_door();
            } else {
                ESP_LOGI("main","未识别到指纹");
            }
        }

        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            printf("BOOT 按键被按下\n");
            zw.Auto_Enroll(&matchID);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}



#ifdef __cplusplus
}
#endif