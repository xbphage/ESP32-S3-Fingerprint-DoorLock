#include "motor.h"
#include "pwm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


void set_angle(uint16_t angle){
    if (angle > 270){
        angle=0;
    }
    uint16_t duty = ((float)angle / 2700 + 0.025 ) * 1024;
    set_duty(duty);
}

// 舵机转到开锁角、保持一会儿再回位。
// 阻塞式实现：这期间 main 循环停转，串口控制台和触摸都不响应。
void open_door(void){
    set_angle(180);

    // 注意单位。vTaskDelay() 收的是 tick 不是毫秒，本工程 CONFIG_FREERTOS_HZ=100，
    // 也就是 1 tick = 10ms —— 写成 vTaskDelay(5000) 会在这里停 50 秒，
    // 表现为"验证通过后设备毫无反应"，而日志停在识别成功那一行、连看门狗都不报
    // （vTaskDelay 是正常让出 CPU 的）。一律用 pdMS_TO_TICKS() 换算。
    vTaskDelay(pdMS_TO_TICKS(5000));

    set_angle(0);
}